# D25-2 S1-R + S2 · 安全性修正与自管 Supervisor

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-09
> 前置：[D25-2 方案设计](D25-2-process-lifecycle-design.md)
> **结论：S1-R 修正完成（发现并修复 2 个真实安全缺陷），S2 实现完成（39 项单测 + 8 项端到端，x86 与板端均 PASS）**

## 0. 结论

| 项目 | 结果 | 依据 |
| --- | --- | --- |
| S1-R 僵尸进程误判 | **已修复** | 第 2.1 节：实测证明原实现会误判 |
| S1-R 临时文件并发冲突 | **已修复** | 第 2.2 节 |
| S2 自管 Supervisor 实现 | **完成** | 第 3 节 |
| S2 单元测试（x86） | **40/40 PASS** | 第 4.1 节 |
| S2 单元测试（板端 ARM） | **40/40 PASS** | 第 4.2 节 |
| S2 端到端（真实 Gateway，板端） | **全项 PASS** | 第 4.3 节 |
| S1 回归（x86 + 板端） | **99/99 PASS** | 第 4.4 节 |
| D23 已验证代码被修改 | **无** | 第 5 节 |

**没有修改任何 D23 已验证的升级流程代码**（`ota_upgrade_run.c`、
`ota_recover.c`、`ota_process.c`、`ota_health.c` 的 `git diff` 均为空）。
本阶段只新增 `ota_supervisor.{c,h}` 及测试。

---

## 1. 为什么要做 S1-R

上一轮 S1 的 75 项测试全部通过，但那组测试**覆盖不到僵尸进程**。
自审时发现 `ota_sup_verify_identity()` 只比对 `starttime`，
没有检查进程状态，这是一个会导致误判的缺陷。

**先写实验证明缺陷真实存在，再改代码**，避免"以为有问题就乱改"。

---

## 2. 发现并修复的缺陷

### 2.1 缺陷一：僵尸进程被误判为 RUNNING（严重）

**实验验证**（宿主机实测）：

```text
僵尸子进程 pid=104276
stat 第3字段(state) = 'Z'
→ /proc 条目仍存在，starttime 仍可读
→ 只比对 starttime 的实现会误判为 RUNNING
```

**原理**：进程退出后若父进程尚未 `waitpid`，它会保持僵尸状态。
此时 `/proc/<pid>/stat` 仍然存在，且 `starttime` **与记录完全一致**。
原实现读到一致的 starttime 就返回 `RUNNING`。

**这个缺陷的实际后果**：

假设 OTA 要停掉旧版 Gateway，而旧版恰好是僵尸：

1. `query()` 返回 RUNNING（错误）。
2. OTA 认为"旧版还在服务"，或尝试对其发 SIGTERM。
3. 对僵尸发信号**无任何效果**（僵尸不响应信号）。
4. 等到超时仍判定"停不下来"，升级流程卡住或走错分支。

更危险的情形：升级后旧版变成僵尸，`query()` 一直说 RUNNING，
OTA 永远无法确认"旧版已退场"，每次升级都会被 `EALREADY`/`EBUSY` 拒绝。

**修复**：新增 `ota_sup_read_state()` 与 `ota_sup_state_is_alive()`，
`verify_identity()` 改为**先看 state，再比 starttime**：

```c
if (ota_sup_read_state(identity->pid, &process_state) != 0) {
    if (errno == ENOENT) return OTA_SUP_STOPPED;
    return OTA_SUP_UNKNOWN;
}

if (!ota_sup_state_is_alive(process_state))
    return OTA_SUP_STOPPED;      /* 僵尸 / 已死亡 */

if (current == identity->starttime)
    return OTA_SUP_RUNNING;
```

状态字符判定：`'Z'`（僵尸）、`'X'`/`'x'`（已死亡）视为非运行；
`R`/`S`/`D`/`T` 等视为运行中（保守）。

**验证测试用例**（`test_zombie_process`）：

```text
  PASS  能读取僵尸进程的 state
  PASS  state 为 'Z'（实际 'Z'）
  PASS  state_is_alive('Z') == 0
  PASS  僵尸的 starttime 仍可读（62522802），故仅比对它不够
  PASS  僵尸 -> STOPPED（实际 STOPPED）
  PASS  写入僵尸身份
  PASS  query -> STOPPED（实际 STOPPED）
  PASS  STOPPED 时不输出 pid
```

其中"僵尸的 starttime 仍可读"这一项是**刻意验证的对照条件**——
它证明问题不在 starttime 本身，而在"只看 starttime"这个判断方式。

### 2.2 缺陷二：临时文件名固定，并发写会互相截断

**原实现**：临时文件名固定为 `gateway.pid.tmp`，用 `O_CREAT | O_TRUNC` 打开。

**问题**：若两个进程同时写 pidfile（例如 OTA Agent 与 init 脚本都在登记），
A 正在写临时文件时 B 以 `O_TRUNC` 打开同一个文件，
A 写入的内容可能被 B 截断，`rename` 之后 pidfile 内容就是坏的。

**修复**：

1. 临时文件名改为 `gateway.pid.tmp.<pid>`，按进程隔离。
2. 改用 `O_CREAT | O_EXCL`，独占创建；若已存在（上次崩溃残留）
   则先删除再重试一次。
3. 剩余部分只允许空白字符（一个换行）。

```c
int tn = snprintf(
    tmp_name, sizeof(tmp_name),
    OTA_SUP_TEMP_PREFIX "%ld", (long)getpid());
```

**注意**：这仍然**不能替代文件锁**。pidfile 的语义是"唯一一份记录"，
真正的并发写只应发生在一个持有 `.ota.lock` 的写者身上。
本修正只是保证"即使误用也不会产生半截内容"。

**验证**：`test_temp_file_isolation` 确认写入后无残留临时文件，
且连续两次写入以后写者生效。

### 2.3 顺带修正的小问题

| 问题 | 原状 | 现状 |
| --- | --- | --- |
| umask 影响权限 | `open(0600)` 受 umask 影响 | `fstat` 校验后必要时 `fchmod` |
| 残留临时文件无法清理 | 固定名，只能删一个 | 扫描 run 目录，删除所有前缀匹配项 |
| `fstat` 失败的分支逻辑 | 嵌套判断混乱 | 拆成两步，逻辑清晰 |

---

## 3. S2：自管 Supervisor

### 3.1 设计原则

**"自管"的定义**：Gateway 由 OTA 自己 `fork` 出来，因此是本进程的子进程。
这是升级过程中"临时拉起候选版本做健康检查"和测试环境的形态。

**与"外部 supervisor"（S5）的区别**：

| | S2 自管 | S5 外部（init 启动） |
| --- | --- | --- |
| 父子关系 | OTA 是父进程 | init 是父进程 |
| 能否 `waitpid` | 能 | **不能**（ECHILD） |
| 停止手段 | `ota_process_stop()` | 发信号 + 由 init 回收 |
| 状态查询 | 可 wait 获知退出码 | 只能靠 pidfile + `/proc` |

### 3.2 接口

```c
typedef struct {
    pid_t pid;
    unsigned long long starttime;
    pid_t owner_pid;    /* 记录启动者，防止跨进程误用 */
} ota_sup_managed_t;

int ota_sup_start_managed(
    const char *install_dir,
    const char *executable,
    char *const argv[],
    ota_sup_managed_t *handle_out);

int ota_sup_stop_managed(
    const char *install_dir,
    const ota_sup_managed_t *handle,
    int timeout_ms,
    int *wait_status,
    long long *elapsed_ms);
```

### 3.3 关键安全决策

**决策一：启动前必须先查状态。**

```c
ota_sup_state_t existing = ota_sup_query(install_dir, NULL);

if (existing == OTA_SUP_RUNNING) { errno = EALREADY; return -1; }
if (existing == OTA_SUP_UNKNOWN) { errno = EBUSY;    return -1; }
```

- `RUNNING` → 拒绝。第二实例大概率会因 7000 端口冲突失败，
  但**此时 pidfile 已被覆盖**，原进程的记录就丢了。
- `UNKNOWN` → 同样拒绝。状态不明时启动新进程，
  完全可能造成两个 Gateway 并存。

**决策二：写 pidfile 失败必须杀掉刚启动的进程。**

```c
if (ota_sup_pidfile_write(install_dir, &identity) != 0) {
    (void)kill(pid, SIGTERM);
    (void)waitpid(pid, NULL, 0);
    errno = saved_errno;
    return -1;
}
```

否则会留下一个"在跑但没有任何记录"的孤儿 Gateway——
后续 `query()` 返回 STOPPED，下次升级会再起一个。

**决策三：stop 必须校验所有者。**

```c
if (handle->owner_pid != getpid()) {
    errno = EPERM;
    return -1;
}
```

跨进程 `waitpid` 会失败（ECHILD），更糟的是可能去操作一个
根本不是自己子进程的 pid。

**决策四：stop 失败时不删 pidfile。**

```c
if (rc < 0)
    return -1;              /* 进程可能仍在，记录必须保留 */

/* 只有确认退出后才清理 */
ota_sup_pidfile_remove(install_dir);
```

若 stop 失败但进程还在，删掉记录会让 `query()` 返回 STOPPED，
误导调用方去启动第二个实例。

**决策五：采集 starttime 失败时回收子进程。**

```c
if (ota_sup_capture_identity(pid, &identity) != 0) {
    (void)kill(pid, SIGKILL);
    (void)waitpid(pid, NULL, 0);
    errno = saved_errno;
    return -1;
}
```

子进程启动后极快退出时会读不到 `/proc`，此时必须回收，
否则立即产生僵尸。**这个场景正好是 S1-R 缺陷一的现实来源。**

---

## 4. 验收结果

### 4.1 S2 单元测试（宿主机 x86-64）

```text
$ make test-supervisor-s2

检查项: 40, 失败: 0
结果: PASS
```

覆盖的 8 组场景：

| # | 场景 | 关键断言 |
| --- | --- | --- |
| 1 | 正常启动 + 停止 | pidfile 身份一致；`query` RUNNING；停止后 STOPPED |
| 2 | 重复启动 | `EALREADY`；原实例 pidfile 未被覆盖 |
| 3 | 非所有者停止 | `EPERM`；真实进程未被误杀 |
| 4 | exec 失败 | 不留 pidfile；状态 STOPPED |
| 5 | 外部 SIGKILL | 僵尸状态仍判 STOPPED |
| 6 | 正常停止 | `waitpid` 返回 ECHILD（无僵尸）；`/proc` 消失 |
| 7 | pidfile 损坏 | `query` UNKNOWN；启动被拒 `EBUSY` |
| 8 | 参数校验 | NULL / pid=1 全部拒绝 |

### 4.2 S2 单元测试（板端 i.MX6ULL）

```text
$ adb shell '/tmp/t2'

检查项: 40, 失败: 0
结果: PASS
```

**板端通过的意义**：证明僵尸检测、`/proc` 语义、
`waitpid` 行为在**目标内核 4.9.88** 上与本实现一致。

### 4.3 端到端验证（真实 ARM Gateway，板端）

不是模拟进程，而是用编译好的 ARM `smart_gateway` 走完整流程：

```text
D25-2 S2 端到端验证
install = /tmp/e2e-inst
gateway = /tmp/gw

== 0. 初始状态 ==
  PASS  初始 STOPPED

== 1. 启动真实 Gateway ==
  PASS  启动成功

== 2. 身份校验 ==
  PASS  query -> RUNNING
  PASS  pid 一致
  PASS  pidfile 可读
  PASS  pidfile 身份与句柄一致

== 3. 重复启动保护 ==
  PASS  第二个实例被拒（EALREADY）
  PASS  原实例未受影响

== 4. 优雅停止 ==
[BT] waitting for Bluetooth worker...
[WIFI] waiting for client workers...
[WIFI] all client workers stopped
[SERVER WORKER] stopped

Gateway stopped
  stop rc=0 elapsed=22ms exit=0
  PASS  SIGTERM 优雅退出（未用 SIGKILL）
  PASS  退出码为 0

== 5. 停止后状态 ==
  PASS  query -> STOPPED
  PASS  pidfile 已清理
  PASS  无僵尸残留（waitpid=ECHILD）
  PASS  /proc 条目已消失

结果: PASS
```

**关键数据**：

- 优雅退出耗时 **22 ms**（`ota_process_stop` 默认超时 3000 ms，
  说明 Gateway 的 SIGTERM 处理干净利落）
- `exit=0`，各 worker 均正常 join
- **未使用 SIGKILL**，说明是真正的优雅停止而非强杀

### 4.4 S1 回归

| 环境 | 检查项 | 结果 |
| --- | --- | --- |
| 宿主机 x86-64 | 99 | PASS |
| 板端 i.MX6ULL | 99 | PASS |

S1 从 75 项增加到 99 项，新增的 24 项是僵尸检测、
状态字符分类、临时文件隔离三组。

### 4.5 x86 全量回归

```text
$ make test
test_crc16 ... test_tcp_send_timeout   全部 PASS
全部单元测试通过
```

---

## 5. 未修改 D23 已验证代码（证明）

```sh
$ git status --short bsp/apps/ota_agent/ota_upgrade_run.c \
                       bsp/apps/ota_agent/ota_process.c \
                       bsp/apps/ota_agent/ota_health.c \
                       bsp/apps/ota_agent/ota_recover.c
（无输出）
```

四个文件零改动。之所以能做到，是因为 S2 采用**封装而非改造**：
`ota_supervisor.c` 调用现有的 `ota_process_start/stop`，
把所有权、身份校验、状态判定的逻辑放在新层，不碰底层实现。

---

## 6. 本阶段改动清单

| 文件 | 类型 | 说明 |
| --- | --- | --- |
| `bsp/apps/ota_agent/ota_supervisor.h` | 修改 | 新增 state 读取接口与 S2 接口 |
| `bsp/apps/ota_agent/ota_supervisor.c` | 修改 | 僵尸检测、临时文件隔离、S2 实现 |
| `bsp/apps/ota_agent/ota_supervisor_test.c` | 修改 | 75 → 99 项 |
| `bsp/apps/ota_agent/ota_supervisor_s2_test.c` | **新增** | 40 项 S2 单测 |
| `bsp/apps/ota_agent/ota_supervisor_e2e.c` | **新增** | 端到端验证程序 |
| `bsp/apps/ota_agent/Makefile` | 修改 | 新增测试目标 |

**未改动**：`ota_upgrade_run.c`、`ota_process.c`、`ota_health.c`、
`ota_recover.c`、`ota_upgrade.c`、`gateway/`、`common/`、`server/`。

---

## 7. 已知限制

| # | 限制 | 说明 |
| --- | --- | --- |
| L1 | `ota_process.c` 在 `-Werror` 下有既存告警 | `(void)write()` 的 `unused-result`。该文件属 D23 已验证代码，本阶段不修改，测试构建时单独加 `-Wno-unused-result` |
| L2 | pidfile 无文件锁保护 | 并发写仍可能产生"后写者胜"，需要调用方持有 `.ota-lock`。S2 只保证不产生半截内容 |
| L3 | 未处理 pid 命名空间 | 容器环境下 `/proc` 的 pid 可能与宿主机不同。当前目标板是裸机，不涉及 |
| L4 | 未接入升级流程 | S3 的工作。本阶段只提供能力，不改调用方 |
| L5 | 端到端未覆盖"启动后立即崩溃" | 单元测试覆盖了该路径（`starttime` 采集失败分支），但未用真实 Gateway 构造 |

---

## 8. 下一步（S3）

S3 的目标是把 supervisor 接入 `ota_upgrade_run.c` 的升级流程，
即设计文档第 5.2 节的 0a–0c 步骤：

| 步骤 | 动作 | 失败处理 |
| --- | --- | --- |
| 0a | `ota_sup_query()` 确认当前状态 | UNKNOWN → 中止升级 |
| 0b | 记录当前 `current` 软链 | 读失败 → 中止 |
| 0c | 若 RUNNING，优雅停止并**确认退出** | 停止失败 → **中止**，不进入切换 |

**S3 是第一个会改到 D23 已验证代码的阶段**，必须：

1. 改动后重跑 D23 的全部用例。
2. 注意 D23 测试依赖 `build/ota-test/` 下的手工构建工具，
   本轮发现这些工具未构建（`ota_baseline_init` 缺失），
   S3 开始前需要先补齐构建流程，否则无法做回归。

**这一条必须记住**：D23 的集成测试脚本 `test_ota_northbound_fault.py`
需要 `--prepare` 参数，且依赖 `build/ota-test/ota_baseline_init`
等辅助二进制。本阶段尝试运行失败，原因是这些工具未构建，
**不是代码回归**。
