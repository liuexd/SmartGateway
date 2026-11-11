# D25-2 S4：故障注入矩阵 F1–F10

## 一、这一阶段做了什么

矩阵不是新设计的，它来自 D25-2 设计文档第 7 节，是这份设计最早的
验收核心。此前一直是"写了但没跑"的状态。

本阶段把它逐条跑通，并在过程中修正了 3 处产品缺陷。

```
$ python3 tests/test_s4_fault_matrix.py
F1：Gateway 忽略 SIGTERM -> SIGKILL 后继续
F2：升级中途 OTA 被 kill -9 -> 检测未完成
F3：新版启动失败 -> 回滚
F4：新版 L1 超时 -> 回滚并恢复旧版
F5：L2 失败 -> 回滚并恢复旧版
F6：回滚后旧版也起不来 -> 明确报告失败
F7：pidfile 指向无关进程 -> 不误杀
F8：pidfile 损坏 -> UNKNOWN -> 中止
F9：升级中掉电 -> current 不悬空
F10：并发 OTA -> 第二个因锁失败
合计 66 项, 通过 66, 失败 0
结论: PASS
```

## 二、矩阵与实现分支的对应关系

每一条故障都必须落到一处**真实的判断分支**上。如果某条故障注入后
程序走的是别的分支，这个测试就是假的。

| # | 注入方式 | 命中的产品分支 | 结果 |
|---|----------|----------------|------|
| F1 | 编译 `signal(SIGTERM,SIG_IGN)` 的进程 + pidfile | `prepare_existing_gateway` 的停止逻辑 | 先修正，后通过 |
| F2 | 静默上游 + `kill -9` OTA 本体 | `ota_oplog` 的启动检测 | 新增功能 |
| F3 | 候选二进制 `chmod 644` | `ota_sup_start_managed` → exec 失败 | 通过 |
| F4 | 外部进程真实占用 7000 | `ota_health_wait_tcp_ready` → L1 超时 | 通过 |
| F5 | `OTA_UPSTREAM_PORT=1`（无人监听） | `probe_l2` → 北向连接失败 | 通过 |
| F6 | rename 替换旧版为"合法 ELF 但不服务" | 回滚后 `run_gateway_health` → L1 超时 | 通过 |
| F7 | pidfile 写真实 pid + 错误 starttime | `ota_sup_verify_identity` 的 starttime 比对 | 通过 |
| F8 | pidfile 写入非数字内容 | `ota_sup_pidfile_read` 的严格解析 | 通过 |
| F9 | 三种掉电时机构造 | `current` + state 的独立一致性 | 通过 |
| F10 | 两个 OTA 同时启动 | `ota_lock_acquire` 的 `LOCK_NB` | 先修正，后通过 |

## 三、修正一：F1 的 SIGKILL 策略（产品代码）

### 3.1 原实现

S3 阶段写的是"停止超时即中止，不发 SIGKILL"，注释给的理由是：

```
不再发 SIGKILL 硬杀：如果它卡在 D 状态，SIGKILL 也无效；
如果不是，只会让它来不及清理资源。
```

### 3.2 这个理由错在哪

它把**两种完全不同的情况**当成了一种：

| 情况 | 成因 | SIGKILL 有效？ |
|------|------|----------------|
| 用户态忽略 SIGTERM | `signal(SIGTERM, SIG_IGN)` 或 handler 死循环 | **一定有效** |
| 不可中断睡眠（D） | 磁盘/NFS I/O 卡死 | 挂起，无效 |

原实现按情况二处理情况一，把一个**可自动恢复**的故障变成了
"必须人工介入"。

实测证据（`/tmp/stubborn` 忽略 SIGTERM）：

```
Pre-upgrade gateway state: RUNNING
NOTICE: stopping running gateway (pid=126542)
FAIL: gateway pid=126542 did not exit within 3000 ms
--- stubborn still alive? ---
存活 => 未发 SIGKILL
```

进程明明一个 `kill -9` 就能收掉，升级却放弃了。

### 3.3 修正后的顺序

```
SIGTERM → 等 3s
   ├─ 退出        → 继续升级
   └─ 未退出      → SIGKILL → 等 2s
                     ├─ 退出        → 继续升级
                     └─ 未退出      → 中止（这才是真正的 D 状态）
```

关键点：**用等待结果区分两种情况，而不是靠猜**。

另外，发 SIGKILL 之前再次校验身份——从上次 `query` 到这里又过了 3 秒，
期间 pid 可能已被复用。对身份不明的 pid 发信号是禁止的。

修正后实测：

```
Pre-upgrade gateway state: RUNNING
NOTICE: stopping running gateway (pid=126718)
NOTICE: SIGTERM ignored; escalating to SIGKILL
PASS: gateway killed after 3050 ms (SIGTERM ignored)
...
PASS: OTA upgrade CONFIRMED: 0.1.1-test1
```

### 3.4 为什么 `KILL_TIMEOUT_MS` 只需 2 秒

SIGKILL 无法被捕获或忽略，正常情况内核下次调度就终止进程。
等 2 秒还没消失，只能是卡在 I/O 里——再多等无益。

## 四、修正二：F2 的中断记录（新增模块）

### 4.1 缺失的能力

`ota_state.json` 是权威状态，但它只在状态机转换时更新。升级过程中
存在若干"已经产生副作用、但还没到下一次状态转换"的时刻：

```
停掉旧版 Gateway  →  ...  → 切换 current  →  启动新版
                   ^
                   |  如果 OTA 在这里被 kill -9 或掉电，
                   |  没有任何记录说明"旧版是被 OTA 停的"
```

下次开机：`state` 仍是 CONFIRMED、`current` 仍指向旧版，看起来一切正常
——但业务其实已经停了，而没有任何一方知道曾经发生过一次未完成的升级。

### 4.2 新增 `ota_oplog`

记录 `{phase, pid, started_at, boot_id}` 到 `<install>/last_operation.json`，
正常结束时写 `DONE`/`FAILED` 后删除。

**职责边界（必须守住）**：

| | 用途 |
|---|---|
| `ota_state.json` + `current` | 权威状态，一切一致性判断都基于它 |
| `last_operation.json` | 辅助信息，只用于"检测到未完成时告警" |

恢复逻辑**不得**依赖 `last_operation` 做"该回滚还是该继续"的决定。
文件系统一致性必须由 state + current 独立验证。

### 4.3 策略：宁可阻塞，不可静默继续

检测到未完成记录时一律**拒绝继续升级**，不自动清理。

理由：残留可能来自"旧版已停、新版未起"这种业务中断状态。自动清理
等价于替运维宣称"上次的残留无害"，而我们无法证明这一点。

但拒绝之后必须有显式解除路径，否则遗留文件会让目录永久无法升级，
最终逼着人手写 `rm`——那比不做检查更糟。因此新增：

```bash
ota_agent ack-operation <install_dir>
```

它只删记录，**不**启动 Gateway、**不**改状态、**不**动版本文件。
语义上等于运维说"我看过了，可以继续"。

### 4.4 一个顺序陷阱（已修）

初版把中断检测放在 `state` 检查**之后**，结果被 `state` 抢先拦下：

```
FAIL: pending OTA transaction      <- 用户看到的是这句
```

真正的原因（`last_operation` 说明上次中断在 STARTING_NEW）被掩盖了。
中断检测必须放在最前面：它的目的是**解释**当前状态为什么可疑，
而不是在状态检查之后补充一句。

### 4.5 验证

```
F2：残留未完成记录: exists=True
F2：记录的 phase 不是终态: phase=STARTING_NEW
F2：拒绝自动继续: exit=1
F2：明确指出中断阶段
F2：给出人工解除路径
F2：ack-operation 成功
F2：记录已清除
F2：解除后不再报未完成
F2：recover 能收敛
F2：current 指向存在的目录: releases/v0.1.0-test1
```

## 五、修正三：F10 的锁冲突提示（产品代码）

原实现：

```c
perror("OTA upgrade lock");
```

输出 `OTA upgrade lock: Resource temporarily unavailable`。

并发升级是**预期会发生**的情形（定时任务 + 人工触发同时发生），
运维看到这句话无法判断是"另一个 OTA 在跑"还是"磁盘/权限问题"，
也不知道该怎么办。

修正后：

```
FAIL: another OTA operation is already in progress
      another process holds .ota.lock in <install>
      wait for it to finish, then retry
```

## 六、测试构造中踩的三个坑

这三个都是测试自己的问题，不是产品缺陷，但会直接导致假 PASS/假 FAIL。

### 6.1 管道全缓冲吞掉输出（F2、F10）

OTA 的 stdout 被管道接走时是全缓冲。用 `readline()` 等它输出，
会一直阻塞到进程退出——而那时**锁已释放、oplog 已清理**。

表现：

- F10 的第二个进程顺利拿到锁，真正失败原因变成
  `candidate equals confirmed`（说明第一个已经跑完了）
- F2 读不到任何行，`saw_switch` 为假

修法：`stdbuf -oL -eL`。

```python
cmd = [str(AGENT), "upgrade-test", str(PKG_NEW), str(install)]
if shutil.which("stdbuf"):
    cmd = ["stdbuf", "-oL", "-eL"] + cmd
```

### 6.2 构造故障时越过了被测分支（F6）

初版直接把旧版二进制写成 `not-an-elf`，结果它连 preflight 都过不了
（SHA 不符），根本走不到回滚路径——测的完全不是 F6。

正确构造：让旧版**能通过完整性校验但不能提供服务**（合法 ELF、
SHA 自洽、启动即退出）。这对应真实场景中的"依赖缺失""配置错误"。

```python
# 编译一个合法 ELF：立即返回
broken = "int main(void){return 0;}"
# 同时更新 manifest 的 size 与 sha256，保持自洽
```

并且要先断言"前置校验确实放行了"，否则无法区分"测到了回滚"
和"被 preflight 拦下了"：

```python
record("F6：前置校验放行（确实走到了回滚）",
       "safe to stop the gateway" in out)
```

### 6.3 `ETXTBSY`：不能写入正在执行的 ELF（F6）

旧版 Gateway 正在运行，其可执行文件的 inode 是打开着的，
`write_bytes` 必然得到 `Text file busy`。

这不是绕过限制的取巧，而是真实的物理约束：**覆盖一个正在执行的
程序必然失败**。正确做法是 `rename` 原子替换——只换目录项，
旧 inode 继续服务已运行的进程，新内容落在新 inode 上。

这恰好正是"磁盘上的发布目录被改坏、而内存中仍在跑旧镜像"的真实形态。

## 七、未能验证的部分（必须明确）

### 7.1 `survived SIGKILL` 分支未做集成测试

修正 F1 后，代码里存在这条分支：

```c
printf("FAIL: gateway pid=%ld survived SIGKILL (%d ms)\n", ...);
puts("      process is likely in uninterruptible (D) state");
```

要触发它需要构造一个 **SIGKILL 也无效**的进程，即卡在 D 状态。
本机无法构造：

| 方式 | 状态 |
|------|------|
| 忽略 SIGTERM | 只能验证第一个分支，SIGKILL 必定杀掉 |
| `SIGSTOP` | SIGKILL 仍会终止（实测：直接消失） |
| `ptrace` 挂住 | SIGKILL 会打成僵尸（实测：`t` → `Z`） |
| FUSE 卡住 I/O | `/dev/fuse` 存在但**缺 fuse3 开发头文件**，pip 不可用 |
| NFS 卡住 I/O | 无 NFS 环境 |
| 直接改进程状态 | 需要改内核内存，无权限 |

按项目规则「不允许把没有实际验证的功能写成 PASS」，该分支列为
**未验证**。它是纯防御性代码：正常情况下不可达，可达时说明
系统已经处于无法自动收敛的状态，此时唯一正确的动作就是中止。

### 7.2 板端行为未经实测

全部 10 条都在 x86 上执行。板端差异主要在两处：

- 4.9.88 的 `/proc` 字段与 x86 一致（已核验 pidfile/starttime 逻辑）
- BusyBox 环境下 `ps` 的行为不同，但本实现不依赖 `ps`，
  全部走 `/proc` 直接读取

板端全量复跑安排在 S6。

## 八、回归

| 项 | 结果 |
|---|---|
| S4 故障矩阵 F1–F10 | **66/66 PASS** |
| D23 北向故障注入 | 27/27 PASS |
| S3 前置校验（9 场景） | 73/73 PASS（场景三已按新行为改写） |
| S3-R4 恢复路径 | 36/36 PASS |
| x86 单元测试 | 全部通过 |
| `test-supervisor` | 145/145 |
| `test-supervisor-s2` | 44/44 |
| ARM 交叉编译 | ELF32 ARM EABI5 hard-float，213400 字节 |

## 九、S3 场景三的连带修正

F1 改变了停止策略，S3 的场景三也必须同步——它原来断言的正是
"不发 SIGKILL"这个已被判定为错误的行为。

如果只改产品不改测试，结果会是"产品对了、测试红了"，然后很容易
被人"顺手"改回去。改写后的断言更有价值，因为它验证的是**顺序性**：

```python
record("确认退出发生在切换版本之前（关键安全约束）",
       exited_idx >= 0 and activated_idx >= 0
       and exited_idx < activated_idx)
```

只断言"最终进程死了"是不够的：一个先切版本、再补杀进程的实现
同样满足"进程死了"，但它已经制造了"旧版在跑、文件被换"的破损状态。

## 十、遗留限制

| 编号 | 内容 |
|---|---|
| L10 | `survived SIGKILL` 分支无集成测试（本机不可构造 D 状态） |
| L11 | `last_operation.json` 固定临时名，当前依赖 `.ota.lock` 串行化；若将来出现非持锁调用点需改 |
| L6 | 停止外部进程的 TOCTOU 窗口（pidfd 在 4.9.88 不可用），S5 由 supervisor 接管所有权后可消除 |
| L3 | 前置校验依赖 pidfile 存在，S5 解决 |
