# D25-2 S3-R4：停止 Gateway 之后的失败路径与恢复

## 一、为什么需要这一阶段

S3-R3 结束时，前置校验已经覆盖了 `ota_activate()` 的全部变更条件。
但前置校验通过之后、`ota_activate()` 真正开始改目录之前，还有几步：

```
preflight_package()          <- 只读
prepare_existing_gateway()   <- **停掉旧版 Gateway（产生副作用）**
preflight_upstream_port()    <- 只读
启 Mock Server                <- 占端口
ota_activate()               <- 开始改目录
```

问题在于 `prepare_existing_gateway()` 之后的三步任何一步失败，
原实现都是裸 `return 1`：

```c
/* 修复前 */
if (prepare_existing_gateway(...) != 0) return 1;   /* 这里还没停 */
if (start_mock_server(...) != 0) return 1;          /* 已经停了！业务中断 */
```

也就是说，旧版 Gateway 已经被 SIGTERM 掉，脚本随即退出，
**没有任何人把旧版拉回来**。板子上表现为：升级命令失败，
业务从此刻起完全停止，直到人工介入。

这不是"升级失败"，这是"升级把一个正在跑的服务搞停了还没恢复"。

## 二、实测复现（修复前）

```bash
OTA_UPSTREAM_PORT=99999 ./ota_agent_x86 upgrade-test <pkg> <install>
```

注意 `OTA_UPSTREAM_PORT` 原本是在 `prepare_existing_gateway()` **之后**
才被解析和校验的，所以：

```
Gateway before upgrade: running (pid=27183)
[stop] gateway stopped
FAIL: invalid OTA_UPSTREAM_PORT: 99999
$ echo $?
1
```

```
$ ss -ltn | grep 7000
（无输出）
$ ps aux | grep smart_gateway
（无输出）
```

旧版停了，新的没装，端口空了，命令返回 1。业务中断。

## 三、修复设计

### 3.1 把"要不要停"和"停完之后失败怎么办"分离

引入上下文结构，显式记录本次运行对系统造成的影响：

```c
typedef struct {
    int gateway_was_running;    /* 升级前是否在跑 */
    int gateway_stopped_by_ota; /* 是否由本次 OTA 停掉 */
    int activation_started;     /* 是否已进入不可回退的激活阶段 */
} ota_upgrade_context_t;
```

三个字段都不是冗余的：

- `gateway_was_running` 决定**失败后该不该启动**。升级前就没跑，
  说明服务本来由别人管（或本就没启用），OTA 失败后擅自拉起属于越权。
- `gateway_stopped_by_ota` 决定**恢复时该不该停**。没停过就无需恢复。
- `activation_started` 决定**能不能回退**。一旦进入激活，目录已经变了，
  不能再走"恢复旧版"这条路，只能继续走既有的事务回滚。

### 3.2 把只读校验提前到停止之前

`preflight_upstream_port()` 从 Mock Server 的启动逻辑中拆出来，
在 `prepare_existing_gateway()` **之前**执行：

```c
if (preflight_package(...) != 0)          return 1;  /* 只读 */
if (preflight_upstream_port(...) != 0)    return 1;  /* 只读，新增 */
if (prepare_existing_gateway(...) != 0)   return 1;  /* 开始产生副作用 */
```

判定标准不是"我检查了什么"，而是"变更阶段还会检查什么"。
停止 Gateway 之后才可能失败的每一个条件，都必须在停止之前先过一遍。

### 3.3 统一失败出口

停止之后的所有失败路径都收敛到一个函数：

```c
static int abort_after_stop(const char *install_dir,
                            const ota_upgrade_context_t *ctx,
                            const char *reason);
```

它做三件事：

1. 打印失败原因；
2. 若 `ctx->gateway_stopped_by_ota`，尝试恢复旧版；
3. 恢复失败时打印 `RECOVERY REQUIRED` 并给出人工操作指引。

### 3.4 恢复的幂等性

`restore_gateway()` 只依赖调用瞬间的 `/proc` 与 pidfile 状态，
不依赖"上一次尝试是否成功"：

```c
if (!ctx->gateway_was_running)   return 0;  /* 本来就不该有 */
if (!ctx->gateway_stopped_by_ota) return 0; /* 没停过 */
```

重复调用两次和调用一次的结果一致，因此可以安全地在多个失败分支复用。

## 四、本次发现并修复的缺陷：恢复判据的假阳性

### 4.1 现象

按上面设计实现后，人为制造"停止后端口被外部进程抢占"，加日志跑一遍：

```
[stop] gateway stopped
RECOVERY: restarting old gateway...
PASS: old gateway restored and L1-ready     <-- 假的
```

但此刻真实情况是：

```
$ ss -ltnp | grep 7000
LISTEN 0 1 0.0.0.0:7000 users:(("python3",pid=27301,fd=3))
```

7000 上监听的是测试脚本自己抢的 socket，不是 Gateway。
OTA 启动的新进程因为 bind 失败**已经退出了**。

### 4.2 根因

恢复判据用的是既有的健康检查：

```c
/* 修复前 */
if (ota_health_wait_tcp_ready(HOST, 7000, 5000) == 0)
    printf("PASS: old gateway restored and L1-ready\n");
```

`ota_health_wait_tcp_ready()` 的语义是"能 connect 上 7000"。
但 connect 成功只证明**有人**在听，不证明**我们启动的那个进程**在听。
外部进程先占了端口，判据就被骗过去了。

更糟的是，这种情况下 `restore_gateway()` 返回 0（成功），
`abort_after_stop()` 因此不会打印 `RECOVERY REQUIRED`，
运维看到的是"PASS"，实际上业务仍然中断。

**假阳性比缺失检查更有害** —— 缺失检查会让知道自己在裸奔，
假阳性会让运维放心地去睡觉。

### 4.3 修复：按 inode 反查监听者

新增 `port_owned_by_pid(port, pid)`：

1. 遍历 `/proc/net/tcp` 找 `local_port == port` 且 `state == 0A (LISTEN)` 的行，
   取出 inode；
2. 遍历 `/proc/<pid>/fd/*` 的 `readlink`，看有没有 `socket:[<inode>]`。

只有两者对上，才认为该端口由指定进程持有。

```c
/* 修复后 */
if (ota_health_wait_tcp_ready(...) != 0) { ...失败... }
if (!port_owned_by_pid(port, sup.pid)) {
    /* 有人监听，但不是我们启动的那个 */
    goto fail;
}
```

修复后同一场景输出变为：

```
[stop] gateway stopped
RECOVERY: restarting old gateway...
RECOVERY FAILED: restarted gateway is not L1-ready
RECOVERY REQUIRED: business is interrupted, manual action needed
```

不再有假 PASS。

### 4.4 这个缺陷原本是怎么被发现的

不是靠读代码，是靠**故意构造一个"恰好能骗过旧判据"的环境**。
写测试时问自己："什么情况下这条断言会通过，但其实事情是坏的？"
端口抢占正是这样一个环境。

## 五、验收结果

### 5.1 四个场景（`tests/test_s3r4_recovery.py`，36 项）

| 场景 | 构造 | 关键断言 | 结果 |
|------|------|----------|------|
| A | `OTA_UPSTREAM_PORT=99999` | 在停止 Gateway **之前**拒绝；原进程仍存活；7000 仍监听 | PASS |
| B | 升级前未运行 + 外部占用 7000 | 报告 not running；失败后 `work_procs()` 为空（不擅自启动） | PASS |
| C | 运行中 + 停止后端口被抢占 | `still occupied after stopping gateway`；**无** `restored and L1-ready`；有 `RECOVERY REQUIRED` | PASS |
| D | 候选目录缺失 | 在停止之前拒绝；原 Gateway 存活；7000 仍监听 | PASS |

```
$ python3 tests/test_s3r4_recovery.py
场景 A：非法上游端口 -> 在停止 Gateway 之前拒绝
场景 B：升级前未运行 -> 失败后不得擅自启动 Gateway
场景 C：停止后端口被抢占 -> 必须报告 RECOVERY REQUIRED
场景 D：停止后 activate 失败 -> 自动恢复旧版
合计 36 项, 通过 36, 失败 0
结论: PASS
```

场景 C 中两条断言是互斥的，必须同时成立：

```python
record("未谎报恢复成功（无假阳性）", not claimed_ok)
record("明确报告需要人工恢复", "RECOVERY REQUIRED" in out)
```

只断言第二条不够——旧实现两条都会失败，但一个"两边都报"的实现
（既打印 PASS 又打印 RECOVERY REQUIRED）会漏过去。所以必须同时断言
"PASS 字样不出现"。

### 5.2 回归

| 项 | 结果 |
|----|------|
| D23 北向故障注入 | 27/27 PASS，事务耗时 5.22s |
| S3 前置校验（9 场景） | 73/73 PASS |
| x86 单元测试 | 全部通过 |
| `test-supervisor` | 145/145 |
| `test-supervisor-s2` | 44/44 |
| ARM 交叉编译 | ELF 32-bit LSB, ARM EABI5, hard-float, 193976 字节 |

## 六、测试隔离的注意事项

`setup()` 里加了一步显式的状态重置：

```python
init = ROOT / "build/ota-test/ota_baseline_init"
if init.exists():
    subprocess.run([str(init), str(WORK / "state")], check=True)
```

原因：模板目录 `install-x86-fault.2e0Csr` 是 git 之外的本地产物，
上一次 D23 测试跑完后它内部状态停在 `ROLLED_BACK`。
四个场景第一次运行时全部报

```
[FAIL] 非法端口：状态未变: state=ROLLED_BACK
```

看起来像被测代码有问题，实际是**测试自身的基线不干净**。
断言"状态未变"必须先确定"变之前是什么"，否则测的是运气。

这与之前两次假阳性是同一类错误的两个方向：
- 之前是"断言永远为真"（检查了不存在的路径）；
- 这次是"断言依赖未知基线"（不知道初始值却断言它没变）。

## 七、遗留限制

| 编号 | 限制 | 说明 |
|------|------|------|
| L8 | `port_owned_by_pid` 不覆盖 IPv6 | 只读 `/proc/net/tcp`，未读 `/proc/net/tcp6`。当前 Gateway 绑 `0.0.0.0`，不影响；若将来支持 IPv6 需补。 |
| L9 | `/proc/net/tcp` 与 `/proc/<pid>/fd` 之间存在极短窗口 | 两次读取之间进程可能已退出/重绑。取"fd 中存在该 inode"为充分条件，不会产生假阳性，只在极窄时间窗内可能假阴性（误报恢复失败），偏向安全侧。 |
| L6 | 停止外部进程仍有 TOCTOU 窗口 | 与 S3-R3 相同，pidfd 在 4.9.88 不可用。计划在 S5 由 Service Supervisor 接管子进程所有权后消除。 |
| L3 | 前置校验依赖 pidfile 存在 | 未改变，S5 解决。 |

## 八、结论

- 停止 Gateway 之后的失败路径不再是裸 `return`，
  统一走 `abort_after_stop()`，按上下文决定是否恢复。
- 只读校验（包完整性 + 上游端口）全部提前到停止之前，
  "停止"成为最后一个可回退的边界。
- 恢复判据从"端口有人听"收紧到"端口由我启动的进程持有"，
  消除了一个会向运维谎报成功的假阳性。
- 升级前未运行时失败不擅自启动，符合"不越权管理不属于自己的进程"。
- 36 项场景验收 + 全部既有测试回归通过，ARM 交叉编译通过。

**已知未做**：恢复本身在板端的实测（本阶段全部在 x86 上完成），
统一放在 S6 的板端 OTA 验证中一起做。
