# D25-2 S3-R3 · 第三轮安全复审修正

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-10
> 前置：[S3-R2 前置校验补全](D25-2-S3R2-preflight-completeness.md)
> **结论：复审 5 项中 3 项已在上轮完成，本轮补齐负向测试覆盖与 PID 复用边界说明**

## 0. 结论

| # | 复审要求 | 上轮状态 | 本轮 |
| --- | --- | --- | --- |
| 1 | 完善 OTA 全部只读前置校验 | ✅ 已完成 | 复核确认（6 项全覆盖） |
| 2 | 补充负向测试 | ⚠️ 仅 2 个场景 | **补齐至 7 个分支全覆盖** |
| 3 | 核对 PID 复用场景，明确保证边界 | ⚠️ 分散记录 | **系统化整理（本文第 2 节）** |
| 4 | 测试清理只操作自建进程 | ✅ 已完成 | 复核确认 |
| 5 | 同步 README 进度 | ✅ 已完成 | 本轮补充 S3-R3 |

测试规模：S3 前置验收 40 → **73** 项。

---

## 1. 负向测试覆盖补齐

### 1.1 补齐前的缺口

`activate_check()` 有 6 个校验分支，但只有 2 个被测到：

| 校验分支 | 补齐前 |
| --- | --- |
| 包损坏（SHA 不符） | ✅ `scenario_bad_package` |
| 候选目录缺失 | ✅ `scenario_candidate_missing` |
| **候选版本 == 已确认版本** | ❌ |
| **`current` 指向非确认版本** | ❌ |
| **已确认版本文件损坏** | ❌ |
| **候选已安装但 SHA 不一致** | ❌ |

后两条容易混淆，实际是不同路径：
- "候选目录缺失" → `ota_verify_package(new_path)` 连文件都打不开
- "候选存在但内容损坏" → 能打开，但摘要比对失败

### 1.2 新增 4 个场景

为避免重复代码，抽出通用驱动 `_run_precheck_scenario()`：

```python
def _run_precheck_scenario(name, mutate, expect_msg,
                           gateway_running=True):
    """mutate(install, pkg) 构造非法环境；共同断言见下。"""
```

共同断言（所有负向场景都必须满足）：

1. 升级被拒绝（退出码非 0）
2. 命中**预期的那条**校验信息（不是随便失败）
3. 出现 `aborted before stopping the gateway`
4. Gateway 仍在运行、7000 仍监听
5. `current` 与状态文件均未变

第 2 条尤其重要：只断言"失败"无法区分"因为预期原因失败"与
"因为别的原因碰巧失败"。

| 场景 | 构造方式 | 期望命中的校验 |
| --- | --- | --- |
| 六 | 包 manifest 的 version 改成已确认版本 | `candidate equals confirmed` |
| 七 | 把 `current` 指向 `releases/v9.9.9-phantom` | `confirmed/current mismatch` |
| 八 | 向已确认版本的二进制追加字节 | `confirmed release invalid` |
| 九 | 向已安装候选的二进制追加字节 | `installed candidate mismatch` |

### 1.3 过程中修正的两个测试自身缺陷

**缺陷一：场景七不该启动 Gateway**

该场景通过破坏 `current` 来构造，而 Gateway 正是从
`current/smart_gateway` 启动的。先坏 `current` 再启动必然失败——
这是测试设计问题，不是产品缺陷。

修正：给 `_run_precheck_scenario` 增加 `gateway_running=False`，
该场景只验证"校验能识别不一致"，不需要真实进程。

**缺陷二：基准值取样顺序错误**

原代码在 `mutate()` **之前**取 `before_current`，
但场景七的 mutate 本来就改了 `current`，于是断言必然失败：

```text
[FAIL] current 未变: releases/v0.1.0-test1 -> releases/v9.9.9-phantom
```

修正为在 mutate **之后**取基准——要把"测试自己造的改动"
和"被测代码造成的改动"区分开。

### 1.4 补齐后实测

```text
场景六：候选版本 == 已确认版本 -> 必须拒绝
[PASS] 命中预期校验：candidate equals confirmed

场景七：current 指向非确认版本 -> 必须拒绝
[PASS] 命中预期校验：confirmed/current mismatch

场景八：已确认版本文件被篡改 -> 必须拒绝
FAIL: binary size mismatch
FAIL: confirmed release invalid
[PASS] 命中预期校验：confirmed release invalid

场景九：候选已安装但内容与包不一致 -> 必须拒绝
[PASS] 命中预期校验：installed candidate mismatch

合计 73 项, 通过 73, 失败 0
结论: PASS
```

**7 个校验分支全部有对应负向用例。**

---

## 2. PID 复用与进程身份的保证边界

复审要求"明确现有机制的保证边界"。这一节把散落在各处的
结论集中说明，**包括哪些做得到、哪些做不到**。

### 2.1 身份三元组

```c
typedef struct {
    pid_t pid;
    unsigned long long starttime;   /* /proc/<pid>/stat 第 22 字段 */
    char boot_id[40];               /* /proc/sys/kernel/random/boot_id */
} ota_sup_identity_t;
```

判定逻辑（`ota_sup_verify_identity`）：

1. `pid <= 1` → **UNKNOWN**（绝不对 init 发信号）
2. 读不到 `/proc/<pid>/stat`：
   - `ENOENT` → **STOPPED**
   - 其他 → **UNKNOWN**
3. 单次读取取 `state` + `starttime`（同一快照，避免组合错配）
4. `state ∈ {Z, X, x}` → **STOPPED**（僵尸/已死亡不算运行）
5. `boot_id` 非空且与当前不同 → **STOPPED**（跨重启，pid 无意义）
6. `starttime` 相同 → **RUNNING**，不同 → **STOPPED**（pid 已复用）

### 2.2 能保证什么

| 场景 | 是否可靠识别 | 依据 |
| --- | --- | --- |
| 进程已退出、pid 未被复用 | ✅ 判 STOPPED | `/proc` 条目消失（ENOENT） |
| 进程退出成僵尸 | ✅ 判 STOPPED | state = 'Z' |
| pid 被复用 | ✅ 判 STOPPED | starttime 不同 |
| 系统重启后旧 pidfile 残留 | ✅ 判 STOPPED | boot_id 不同 |
| pidfile 内容被篡改 | ✅ 判 UNKNOWN | 格式严格校验（含 UUID 格式） |
| pidfile 是符号链接 | ✅ 判 UNKNOWN | `O_NOFOLLOW` → ELOOP |
| 目标是 init（pid 1） | ✅ 判 UNKNOWN | 显式拒绝 |

**判定层面是可靠的**：`query()` 不会把"已死进程"或"别的进程"
误报成 RUNNING。

### 2.3 不能保证什么（重要）

| 场景 | 结果 | 原因 |
| --- | --- | --- |
| **`query()` 与 `kill()` 之间的 pid 复用** | ⚠️ **可能误发信号** | 见下 |
| 同一 tick 内 fork 的多个进程 | 无法用 starttime 区分 | starttime 精度是 clock tick（已实测同值） |
| 容器/pid 命名空间 | 未处理 | 当前目标是裸机，不涉及 |

**关于 starttime 精度的实测证据**：

```text
pid=66912 starttime=34184254
pid=66913 starttime=34184254   ← 同值
```

因此唯一性**必须**来自 pid，starttime 只承担"检测 pid 是否被复用"。

### 2.4 TOCTOU：本项目的明确边界

这是唯一**无法在本项目内彻底解决**的问题。

**竞争窗口**：

```c
ota_sup_query(install_dir, &running_pid);   // 读到 pid + 身份
                                            // ← 窗口：旧进程退出，
                                            //    pid 被内核复用
kill(running_pid, SIGTERM);                 // 可能打到无关进程
```

**为什么不能彻底解决**：

标准解法是 `pidfd_open()` + `pidfd_send_signal()`——打开进程句柄后，
信号必然送达同一进程，内核层面消除竞争。

但板端实测：

```text
$ ls /proc/sys/kernel/ | grep -i pidfd
（无输出）
$ uname -r
4.9.88
```

pidfd 需要 **Linux 5.1+**，板端是 **4.9.88**，**不可用**。

**现有缓解**（缩小窗口，不消除）：

1. 发信号**前**重新取一次快照；若已 STOPPED 则不发信号
2. 发信号**后**立刻复核；若变 UNKNOWN 则中止并要求人工核查
3. 整个流程在 `.ota.lock` 排他锁内，排除"另一个 OTA 并发操作"

**风险量化**：

| 因素 | 评估 |
| --- | --- |
| 窗口长度 | 两次 `query` 之间，需完成进程退出 + pid 分配 |
| pid 空间 | `pid_max = 32768`，回绕周期较长 |
| 是否为零 | **不是零** |

**结论表述**：这是**已缓解但未解决**的问题。
文档和提交信息中不得表述为"已修复"。

### 2.5 边界总结表

| 能力 | 保证级别 |
| --- | --- |
| 判断"pidfile 记录的进程是否还在" | **强保证**（三元组 + 僵尸检测） |
| 判断"这个 pid 是否还是当初那个进程" | **强保证**（starttime + boot_id） |
| 向该进程发送信号时"保证不误伤" | **弱保证**（TOCTOU 无法消除） |

第三行是唯一的弱项。**它影响的是"停止操作"，不影响"判断状态"**——
即 `query()` 的结论可信，但基于该结论的写操作存在极小的竞态风险。

### 2.6 为什么接受这个风险

1. 目标板无 pidfd，没有更好的技术手段；
2. 窗口极小且需要特定时序才能触发；
3. 误发 SIGTERM 的后果通常可恢复（目标进程若忽略该信号则无影响）；
4. **S5 会从架构上移走这条路径**：由 init 脚本负责 stop，
   OTA 只调用脚本并检查结果，不再自己 `kill` 外部 pid。

第 4 点是正解。S5 之后，这条风险将不再存在于 OTA 侧。

---

## 3. 其余三项复核

### 3.1 只读前置校验完整性（第 1 项）

`activate_check()` 六项确认无遗漏：

| # | 检查 | 失败信息 |
| --- | --- | --- |
| 1 | 无进行中事务 | `OTA transaction already active` |
| 2 | `current` 指向已确认版本 | `confirmed/current mismatch` |
| 3 | 升级包合法 | verify 内部信息 |
| 4 | 候选 ≠ 已确认 | `candidate equals confirmed` |
| 5 | 已确认版本完整且自洽 | `confirmed release invalid` |
| 6 | 候选已安装且 SHA 一致 | `installed candidate mismatch` |

与 `ota_activate_test()` **共用同一实现**，不会分叉。

### 3.2 测试清理（第 4 项）

确认当前实现满足要求：

```python
_owned_procs = []            # 只登记本测试启动的进程

def cleanup(proc=None, work=True):
    for pid, starttime in _owned_procs:
        if not is_same_process(pid, starttime):
            continue         # pid 被复用则跳过
        os.kill(pid, signal.SIGTERM)
    _owned_procs.clear()
```

- 不扫描 `/proc`
- 不按文件名匹配
- 每次发信号前用 `(pid, starttime)` 复核

残留检查也改为"可执行文件是否位于测试目录下"，
已实测无关 Gateway 不会被误杀。

### 3.3 README（第 5 项）

已包含 S3 / S3-R / S3-R2 三层修正记录，本轮追加 S3-R3。

---

## 4. 验收

| 测试 | 结果 |
| --- | --- |
| D23 回归 | **27/27** PASS，5.23s |
| S3 前置验收 | **73/73** PASS |
| x86 单元测试 | 全部通过 |
| `test-supervisor` | 145/145 PASS |
| `test-supervisor-s2` | 44/44 PASS |

场景数从 5 增至 9，检查项从 40 增至 73。

---

## 5. 改动清单

| 文件 | 改动 |
| --- | --- |
| `tests/test_s3_preflight_running.py` | 新增 `_manifest_for()`、`_run_precheck_scenario()`；新增场景六~九；修正场景七的启动与基准取样 |
| `bsp/docs/D25-2-S3R3-*.md` | 本文件 |
| `bsp/README.md` | 追加 S3-R3 |

**产品代码无改动**——本轮只补测试与文档，因为校验实现已在 S3-R2 完成。

---

## 6. 已知限制（更新）

| # | 限制 | 状态 |
| --- | --- | --- |
| L1 | 停止超时后不发 SIGKILL | 不变 |
| L2 | 旧格式迁移依赖下一次成功启动 | 不变 |
| L3 | 前置检查依赖 pidfile 存在 | 不变，**S5 必须解决** |
| L4 | 未在板端验证"停不下来"场景 | 不变 |
| L5 | 无 CI | 不变 |
| L6 | 外部进程停止存在 TOCTOU 窗口 | **本文件第 2 节系统化说明**，S5 应改由 init 停止 |
| **L7** | **starttime 精度不足以单独作唯一标识** | 新增说明，必须与 pid 组成二元组 |

---

## 7. 下一步

按复审建议的顺序：

1. ~~S3-R2 修正~~（已完成）
2. **S4：故障注入矩阵**（F1–F10）
3. **S5：BusyBox init 集成**（解决 L3 与 L6）

S5 的价值在于**同时**消除两项限制：
- L3：init 脚本写 pidfile → 前置检查不会漏判
- L6：由 init 执行 stop → OTA 侧不再有 TOCTOU
