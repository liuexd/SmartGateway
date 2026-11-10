# D25-2 S3-R · 复审问题修正

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-10
> 前置：[S3 Supervisor 接入](D25-2-S3-supervisor-integration.md)
> **结论：复审指出的 3 个问题全部确认属实；2 个已修复，1 个已缓解并明确记录为不可完全消除**

## 0. 结论

| # | 复审问题 | 核实 | 处理 |
| --- | --- | --- | --- |
| 1 | 停止旧版时机太早（损坏包导致业务中断） | **属实，已复现** | **已修复**：包校验前置 |
| 2 | `query` 与 `kill` 之间 TOCTOU | **属实** | **已缓解**（缩小窗口 + 双重复核），并记录为已知限制 |
| 3 | 测试清理可能误杀无关 Gateway | **属实** | **已修复**：只清理自身启动且身份复核通过的进程 |

测试规模：S3 前置验收 23 → **31** 项。

---

## 1. 问题一：停止旧版的时机太早（已修复）

### 1.1 复现证据

先构造场景：Gateway 正常运行 + 损坏的升级包（SHA-256 故意错误）。

**修复前实测**：

```text
Gateway 启动: pid=118668  7000监听=True

--- 用损坏包执行升级 ---
（Gateway 打印完整退出流程）
Gateway stopped
...
=== 结果 ===
升级退出码: 1
原 Gateway 仍在运行: False      ← 业务已中断
7000 仍在监听: False            ← 端口已释放
current: releases/v0.1.0-test1  ← 版本没变
```

**这是一次纯粹的、本可避免的业务中断**：什么都没升级成功，但正在服务的 Gateway 被停掉了，而且没有任何代码负责把它拉回来。

### 1.2 根因

修复前的执行顺序：

```c
ota_state_load(...);                  // 1. 检查状态
prepare_existing_gateway(...);        // 2. 停旧版   ← 副作用已经发生
wifi_port_busy();                     // 3. 查端口
getenv("OTA_UPSTREAM_PORT");          // 4. 查参数
create_mock_server(...);              // 5. 建上游
ota_activate_test(...);               // 6. 内部才校验包 ← 失败太晚
```

`prepare_existing_gateway()` 是**有副作用**的（停止进程），
却被放在了所有校验之前。

失败后走的 `rollback` 分支也无法挽回：此时 `state` 仍是 `CONFIRMED`，
`ota_recover_execute()` 发现旧版本来就是当前版本，无需回滚；
随后代码发现状态不是 `ROLLED_BACK` 便直接退出。

### 1.3 修复：把校验全部前置

新增 `preflight_package()`，在**任何副作用之前**执行：

```c
static int preflight_package(
    const char *package_dir,
    const char *install_dir)
{
    if (ota_verify_package(package_dir) != 0) {
        puts("FAIL: package verification failed");
        puts("      upgrade aborted before stopping the gateway");
        return -1;
    }

    /* 确认 install/releases 目录可访问 */
    ...
    puts("PASS: preflight checks done; safe to stop the gateway");
    return 0;
}
```

新的执行顺序：

```c
ota_state_load(...);                  // 1. 检查状态
preflight_package(...);               // 2. 校验包（无副作用）
prepare_existing_gateway(...);        // 3. 停旧版（此刻才产生副作用）
wifi_port_busy();                     // 4. 查端口
...
```

**核心原则**：先确认"能不能升级"，再决定"值不值得停掉业务"。

### 1.4 修复后实测

```text
Gateway 启动: pid=118668  7000监听=True

--- 用损坏包执行升级 ---
FAIL: package verification failed
      upgrade aborted before stopping the gateway
FAIL: SHA-256 mismatch

=== 结果 ===
升级退出码: 1
原 Gateway 仍在运行: True      ← 业务未中断
7000 仍在监听: True
current: releases/v0.1.0-test1
```

**这一次同样是失败，但代价为零**——用户会看到明确的"包有问题"提示，
而正在运行的服务完全不受影响。

### 1.5 固化为测试用例

新增 `scenario_bad_package`（场景四）：

```text
[PASS] Gateway 已启动并监听 7000
[PASS] 已写入 pidfile
FAIL: SHA-256 mismatch
      upgrade aborted before stopping the gateway
[PASS] 升级被拒绝: exit=1
[PASS] 在停止 Gateway 之前就拒绝了
[PASS] 原 Gateway 仍在运行（业务未中断）: pid=119026
[PASS] 7000 仍在监听
[PASS] current 未变: current=releases/v0.1.0-test1
[PASS] 状态未变: state=CONFIRMED
```

### 1.6 板端验证

在 i.MX6ULL 上重跑同一场景：

```text
gw=32515 pidfile=32515 34340579 0ccda0b1-...
=== 损坏包升级 ===
FAIL: SHA-256 mismatch
      upgrade aborted before stopping the gateway
=== 关键检查 ===
  OK 原 Gateway 仍在运行
  OK 7000 仍监听
  current=releases/v0.1.0-test1
```

---

## 2. 问题二：`query` 与 `kill` 之间的 TOCTOU（已缓解，不可完全消除）

### 2.1 问题的本质

```c
ota_sup_query(install_dir, &running_pid);   // 读到 pid 与身份
                                            // ← 竞争窗口：进程可能退出，
                                            //    pid 被内核复用
kill(running_pid, SIGTERM);                 // 可能打到无关进程
```

`(pid, starttime, boot_id)` 三元组解决的是**判断身份**的问题，
但**发送信号**这一步没有与身份校验原子化。

### 2.2 为什么不能彻底解决

标准解法是 `pidfd_open()` + `pidfd_send_signal()`：
打开进程句柄后，信号必然送达同一进程，内核层面消除竞争。

**但板端内核是 Linux 4.9.88，已实测确认不支持 pidfd**
（pidfd 需要 5.1+，`/proc/sys/kernel` 下无相关条目）。

因此**无法**在本项目中做到零竞争。这一点必须如实记录，而不是假装解决了。

### 2.3 采取的缓解措施

**措施一：发信号前重新取快照**

```c
ota_sup_state_t recheck = ota_sup_query(install_dir, &running_pid);

if (recheck == OTA_SUP_STOPPED) {
    puts("PASS: gateway exited before signal");
    return 0;                    // 已自己退出，不必发信号
}

if (recheck != OTA_SUP_RUNNING) {
    puts("FAIL: gateway state changed to UNKNOWN before signal");
    return -1;                   // 状态不明，不冒险
}
```

**措施二：发信号后立刻复核**

```c
ota_sup_state_t after_signal = ota_sup_query(install_dir, NULL);

if (after_signal == OTA_SUP_UNKNOWN) {
    puts("FAIL: identity became UNKNOWN right after SIGTERM");
    puts("      cannot confirm the signal reached the gateway");
    return -1;
}
```

若信号发出后身份变得不可判定，就中止升级并要求人工核查——
此时无法确认信号究竟送达了谁，继续操作风险更大。

**措施三：依赖已有的锁**

整个前置检查在 `.ota.lock` 排他锁内执行，
排除了"另一个 OTA 同时操作同一安装目录"这一类竞争。

### 2.4 剩余风险（明确记录）

| 项 | 评估 |
| --- | --- |
| 窗口大小 | 两次 `query` 之间，需完成"旧进程退出 + pid 分配" |
| pid 空间 | `pid_max = 32768`，回绕需较长时间 |
| 是否为零 | **不是**。属于已知限制 |

**结论**：缓解措施能把窗口压缩到极小，但**不能证明不存在**。
彻底解决需要内核支持（pidfd）或改变架构（S5 由 init 管理停止流程）。

### 2.5 对 S5 的输入

复审建议在 S5 设计 init 管理接口时一并考虑外部停止机制，
这是正确的方向：

- 若由 init 脚本负责 `stop`，OTA 只需调用脚本并检查结果，
  不再自己 `kill` 一个外部 pid；
- 进程所有权与信号发送都由 init 这一层完成，
  OTA 侧不需要承担 TOCTOU 风险。

因此 S5 应优先实现"经由 init 停止"，把这条路径上的竞争彻底移走。

---

## 3. 问题三：测试清理可能误杀无关 Gateway（已修复）

### 3.1 原实现

```python
def cleanup(proc=None):
    ...
    # 兜底：杀掉任何遗留的 Gateway
    for pid in gateway_procs():        # ← 扫描整个 /proc
        os.kill(pid, 15)               # ← 按文件名后缀匹配后全部 SIGTERM
```

`gateway_procs()` 遍历 `/proc`，把所有可执行文件以 `/smart_gateway`
结尾的进程都列出来。

**后果**：开发机上若还跑着另一个手工调试的 Gateway，
测试收尾会把它一起杀掉——这是明确的误伤。

### 3.2 修复：只清理自己启动的进程

```python
_owned_procs = []

def own_process(proc):
    """登记由本测试启动的进程（pid + starttime）。"""
    st = read_starttime(proc.pid)
    _owned_procs.append((proc.pid, st))
    return proc

def cleanup(proc=None, work=True):
    ...
    for pid, starttime in _owned_procs:
        if not is_same_process(pid, starttime):
            continue          # pid 已复用或进程已消失，跳过
        os.kill(pid, signal.SIGTERM)
    _owned_procs.clear()
```

四条原则：

1. 只处理显式传入的 `proc`；
2. 只处理本测试登记过的 `_owned_procs`；
3. 每次先用 `(pid, starttime)` 复核身份，**pid 被复用则跳过**；
4. **绝不**全局扫描 `/proc` 后批量发信号。

### 3.3 残留检查也改为目录限定

原来用文件名后缀匹配判断"是否有遗留 Gateway"，
现在改为判断"可执行文件是否位于本次测试目录下"：

```python
def leftover_in_work_dir():
    work_real = str(WORK.resolve())
    ...
    base = exe.replace(" (deleted)", "")
    if base.startswith(work_real):
        found.append(int(entry.name))
```

这样即使机器上有别的 Gateway，也不会影响本测试的残留判断。

### 3.4 实测验证

构造一个"无关"的 Gateway（可执行文件在 `/tmp`，不在测试目录）：

```text
无关 Gateway pid=119049 运行中=yes
--- 运行完整测试（含 cleanup）---
--- 检查无关 Gateway 是否幸存 ---
  ✓ 无关 Gateway 仍存活（未被误杀）
```

修复前这一项必然失败（会被 `gateway_procs()` 命中并杀掉）。

---

## 4. 验收

### 4.1 测试规模

| 测试 | S3 | S3-R |
| --- | --- | --- |
| S3 前置验收 | 23 | **31** |

新增 8 项：场景四（损坏包 + 业务不中断）。

### 4.2 全部结果

| 测试 | 结果 |
| --- | --- |
| D23 回归 | **27/27** PASS，耗时 5.18s |
| S3 前置验收 | **31/31** PASS |
| x86 单元测试 | 全部通过 |
| `test-supervisor` | 145/145 PASS |
| `test-supervisor-s2` | 44/44 PASS |

D23 耗时与 S3 的 5.18s 一致，说明新增 `preflight_package()`
没有引入可测量的开销（包校验本来就在 `ota_activate_test` 里做，
现在只是提前了）。

### 4.3 板端验证

| 项 | 结果 |
| --- | --- |
| 损坏包被提前拒绝 | PASS |
| 原 Gateway 未受影响 | PASS |
| 7000 仍监听 | PASS |
| `current` 未变 | PASS |

---

## 5. 改动清单

| 文件 | 改动 |
| --- | --- |
| `bsp/apps/ota_agent/ota_upgrade_run.c` | 新增 `preflight_package()`；调整执行顺序；kill 前后双重身份复核；新增 `ota_verify.h` 包含 |
| `tests/test_s3_preflight_running.py` | 新增场景四；清理逻辑改为只处理自有进程；残留检查改为目录限定；补 `signal` 导入 |

---

## 6. 已知限制（更新）

| # | 限制 | 状态 |
| --- | --- | --- |
| L1 | 停止超时后不发 SIGKILL | 不变（有意为之） |
| L2 | 旧格式迁移依赖下一次成功启动 | 不变 |
| L3 | 前置检查依赖 pidfile 存在 | 不变，S5 必须解决 |
| L4 | 未在板端验证"停不下来"场景 | 不变 |
| L5 | 无 CI | 不变 |
| **L6** | **外部进程停止存在 TOCTOU 窗口，板端无 pidfd 无法消除** | **新增，已缓解** |

**L6 的处置**：S5 应改为"经由 init 停止"，从架构上移走这条竞争路径。

---

## 7. 关于复审中"单纯复查只能缩小窗口"的判断

复审指出：

> 单纯增加一次身份复查只能缩小窗口，不能彻底消除竞争。

**这个判断完全正确**，本修正也**不声称**解决了问题。
文档中明确写了：

- pidfd 需要 Linux 5.1+，板端是 4.9.88，**不可用**；
- 剩余风险**不是零**；
- 彻底解决需要内核支持或架构调整（S5）。

把"已验证"和"已缓解"分开表述，比含糊地说"修好了"更负责。
