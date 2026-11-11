# D25-2 S2-R · 复审问题修正

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-09
> 前置：[S1-R + S2 验收](D25-2-S1R-S2-supervisor.md)
> **结论：复审指出的 4 个问题全部确认属实并已修正；修正过程中又发现 1 个新缺陷**

## 0. 结论

| # | 复审指出的问题 | 核实结果 | 处理 |
| --- | --- | --- | --- |
| 1 | 临时文件测试路径错误（少 `/run`） | **属实，且是假阳性** | 已修正 + 加反向验证 |
| 2 | "立即退出"测试与文档不一致 | **属实** | 已补真实测试 + 修正注释 |
| 3 | state 与 starttime 分两次读有竞争窗口 | **属实** | 合并为单次 `probe_process` |
| 4 | 缺 boot_id 跨重启校验 | **属实** | 已实现 + 兼容旧格式 |
| 5 | 启动失败时无界 `waitpid` | **属实** | 改为有界回收 `reap_bounded` |
| — | **（新发现）boot_id 解析过于宽松** | 由测试暴露 | 已加严格 UUID 格式校验 |

**测试规模变化**：
- S1 测试：99 → **130** 项
- S2 测试：40 → **44** 项
- 宿主机与板端 i.MX6ULL **均全部 PASS**

---

## 1. 问题一：这是一个假阳性测试（最严重）

### 1.1 原代码

```c
int dn = snprintf(dir, sizeof(dir), "%s/run", g_dir);   // 构造了 run 路径
// ... 但检查用的是：
snprintf(cmd, sizeof(cmd),
    "ls '%s'/gateway.pid.tmp.* 2>/dev/null | wc -l", g_dir);  // 少了 /run
```

实际临时文件位于 `<install>/run/gateway.pid.tmp.<pid>`，
但检查的是 `<install>/gateway.pid.tmp.*`。

### 1.2 实测证明是假阳性

```text
实际文件: gateway.pid.tmp.12345        （在 run/ 目录下）
测试当前的 ls '<install>'/gateway.pid.tmp.* 结果: 0
→ 恒为 0，测试永远通过（即使残留文件真实存在）
```

**这比"没有测试"更糟**：它给出虚假的安全感，
让人以为"临时文件清理已验证"，实际上什么都没验证。

### 1.3 修正

1. 检查路径改为 `<install>/run/gateway.pid.tmp.*`。
2. **额外加一条反向验证**：人为放一个临时文件，确认检查**能发现它**。

```c
/* 反向验证：手动放一个临时文件，确认检查能发现它 */
snprintf(planted, sizeof(planted),
         "%s/gateway.pid.tmp.99999", run_dir);
// 写文件 -> 再跑同一条检查 -> 必须返回 1
CHECK(count == 1, "检查方法能发现人为残留");
```

**为什么这一步重要**：只验证"没有残留"无法区分
"确实没有"与"检查方法本身失效"。反向验证才能证明检查有效。

### 1.4 同时补上真正的并发写测试

原测试的 A/B 两次写入是**顺序执行**的，不是并发。
新增 `test_concurrent_writers`：

```c
for (int i = 0; i < 4; i++) {
    pid_t p = fork();
    if (p == 0) {
        for (int r = 0; r < 50; r++) {
            ota_sup_identity_t id = { ... };
            (void)ota_sup_pidfile_write(g_dir, &id);
        }
        _exit(0);
    }
}
```

4 个进程 × 50 轮 = 200 次并发写，然后验证：

```
PASS  并发写后 pidfile 仍可完整解析
PASS  pid 合法
PASS  starttime 合法
PASS  并发后无临时文件残留
```

**注意**：这**不证明**并发写是安全的（那需要 `.ota.lock`）。
它只证明"不会产生半截或错乱内容"——即 S1-R 修临时文件名隔离的直接效果。

---

## 2. 问题二：测试与文档不一致

### 2.1 事实核对

| 位置 | 声称 | 实际 |
| --- | --- | --- |
| `ota_supervisor_test.c` 文件头 | "10. 启动后立即退出" | `main()` 中**无此测试** |
| 验收文档 L5 | "单元测试覆盖了该路径" | 仅覆盖了 `capture_identity` 失败分支，未构造真实场景 |

### 2.2 修正

1. **文件头注释改为与 `main()` 实际调用一一对应**，
   并明确写出该场景由哪个文件覆盖。
2. **新增真实测试** `test_immediate_exit`（在 S2 测试中）：

```c
/* /bin/true 会立刻退出 */
char *argv[] = { "/bin/true", NULL };
rc = ota_sup_start_managed(g_dir, "/bin/true", argv, &h);
```

验证三条：

| 断言 | 意义 |
| --- | --- |
| 未留下无效 pidfile | 不能写入指向已死进程的记录 |
| 无僵尸残留 | 父进程必须回收子进程 |
| 耗时 ≤ 5s | 不能因进程不响应而永久阻塞 |

板端实测结果：

```text
== 9. 启动后立即退出 ==
  PASS  启动成功（进程可能已退出）
  PASS  立即退出后 -> STOPPED（实际 STOPPED）
  PASS  无僵尸残留（实际 0 个）
  PASS  未出现长时间阻塞（耗时 0s）
```

---

## 3. 问题三：两次读取的竞争窗口

### 3.1 原实现

```c
ota_sup_read_starttime(pid, &current);       // 第一次读 /proc/<pid>/stat
ota_sup_read_state(pid, &process_state);     // 第二次读 /proc/<pid>/stat
```

### 3.2 竞争窗口分析

1. 第一次读取时，pid 对应**旧 Gateway**，得到 `old_starttime`。
2. 旧进程退出，pid 被内核复用给新进程。
3. 第二次读取时，读到的是**新进程**的 state。
4. 最终判断用 `old_starttime` + `new_state`。

这个组合**不对应任何真实存在的快照**：
它既不是"旧进程"（state 来自新进程），
也不是"新进程"（starttime 来自旧进程）。

**具体危害**：若旧 starttime 恰好与新记录相同（理论上可能），
或新进程状态为 'S'，判断结果可能与实际情况相反。

### 3.3 修正：单次读取

新增 `ota_sup_probe_process(pid, &state, &starttime)`，
内部只读一次 `/proc/<pid>/stat`，同时解析两个字段。

同时把三个读取函数统一到同一个核心 `read_stat_fields()`：

```
read_stat_fields()  ← 唯一访问 /proc 的地方
    ├── ota_sup_read_state()        （只要 state）
    ├── ota_sup_read_starttime()    （只要 starttime）
    └── ota_sup_probe_process()     （两者都要）
```

**好处**：
1. 消除快照错配。
2. 三个接口的解析逻辑不可能分叉（原来 `read_state` 和
   `read_starttime` 是两份几乎相同的代码，改一处容易漏另一处）。

`verify_identity()` 现改为：

```c
if (ota_sup_probe_process(
        identity->pid, &process_state, &current) != 0) { ... }
```

新增测试 `test_probe_consistency` 验证两个字段与单独读取一致。

---

## 4. 问题四：boot_id 跨重启校验

### 4.1 为什么 (pid, starttime) 不够

`(pid, starttime)` 只在**同一启动周期内**有效。
重启后：

- 内核从头分配 pid；
- jiffies（starttime 的来源）也从 0 重新计。

于是"旧 pidfile 的 pid + starttime"完全可能在新周期中**碰巧匹配**
到一个无关进程 —— 例如重启后很快启动的某个服务。

而 `/opt` 已实测跨重启持久（D25-1B），pidfile 会保留下来，
这个误匹配是**现实可能**而非理论问题。

### 4.2 实现

身份结构新增 boot_id：

```c
#define OTA_SUP_BOOT_ID_MAX 40

typedef struct {
    pid_t pid;
    unsigned long long starttime;
    char boot_id[OTA_SUP_BOOT_ID_MAX];
} ota_sup_identity_t;
```

来源：`/proc/sys/kernel/random/boot_id`（36 字符 UUID，每次启动重新生成）。

校验逻辑：

```c
if (identity->boot_id[0] != '\0') {
    if (ota_sup_read_boot_id(current_boot_id, ...) != 0)
        return OTA_SUP_UNKNOWN;      /* 读不到，不敢断言 */

    if (strcmp(identity->boot_id, current_boot_id) != 0)
        return OTA_SUP_STOPPED;      /* 跨重启，pid 在本周期无意义 */
}
```

### 4.3 向后兼容（重要）

pidfile 格式演进：

| 格式 | 示例 | 校验维度 |
| --- | --- | --- |
| 旧（两段） | `12345 678\n` | (pid, starttime) |
| 新（三段） | `12345 678 <uuid>\n` | (pid, starttime, boot_id) |

读取端**两种都接受**。理由：升级后若遗留旧 pidfile，
不能因为格式变化就判为损坏——那会导致启动被 `EBUSY` 永久拒绝。

写入端：`boot_id` 为空时自动写旧格式。

### 4.4 降级策略

若 `/proc/sys/kernel/random/boot_id` 不可读（极少数内核配置）：

- `capture_identity` **不失败**，只是 `boot_id` 留空；
- 校验退化为 `(pid, starttime)`；
- 这是一个**能力削弱**，应在验收时记录该限制。

实测板端内核 4.9.88 **支持**该文件，长度 36 字符，往返一致。

---

## 5. 问题五：无界 waitpid

### 5.1 原实现

```c
(void)kill(pid, SIGKILL);
(void)waitpid(pid, NULL, 0);      // 无界阻塞
```

### 5.2 风险

`waitpid(pid, NULL, 0)` 会**永久阻塞**直到子进程退出。
若子进程处于 **D 状态**（不可中断睡眠，常见于 NFS/块设备 I/O 卡死），
SIGKILL 也无法让它退出，父进程就此挂死。

**后果**：OTA Agent 卡在启动失败路径上，整个升级流程无法继续，
比"留下一个僵尸"严重得多。

### 5.3 修正：有界回收

```c
static int reap_bounded(pid_t pid, int timeout_ms)
{
    /* 轮询 waitpid(WNOHANG)，最多等待 timeout_ms */
    /* 返回 0 已回收；1 超时未回收；-1 系统调用错误 */
}
```

调用处改为：

```c
(void)kill(pid, SIGKILL);
(void)reap_bounded(pid, 2000);       /* 而不是无限等 */
```

**取舍说明**：超时后会遗留一个僵尸。
但僵尸会在 OTA 进程自身退出时由 init 接管回收，
这远好于让整个升级流程挂死。

这不是完美方案，是在"必然挂死"与"可能短暂遗留僵尸"之间选择后者。

---

## 6. 修正过程中新发现的缺陷

### 6.1 boot_id 解析过于宽松（由测试暴露）

加 boot_id 后，原有测试用例 `"12345 678 999"` **从"应拒绝"变成"通过"**：

```text
FAIL  拒绝损坏内容 #7: "12345 678 999"
FAIL  query -> UNKNOWN（实际 STOPPED）
```

### 6.2 为什么这是危险的

原解析把 `999` 当成 boot_id 接受。于是：

1. `verify_identity` 发现 boot_id 不匹配（`999` ≠ 真实 UUID）；
2. 返回 **STOPPED**；
3. 但 pid 12345 可能**真的**有一个 Gateway 在跑；
4. OTA 认为"没在运行"，**启动第二个实例**。

正确行为应是：格式非法 → **UNKNOWN** → **中止操作**。

### 6.3 修正：严格 UUID 格式校验

```c
/* 必须恰好 36 字符，且符合 8-4-4-4-12 的 UUID 格式 */
if (boot_len != 36) { errno = EINVAL; return -1; }

for (i = 0; i < boot_len; i++) {
    if (i == 8 || i == 13 || i == 18 || i == 23) {
        if (c != '-') { errno = EINVAL; return -1; }
    } else if (!is_hex(c)) {
        errno = EINVAL;
        return -1;
    }
}
```

**原则**：宁可判定"记录损坏"并停下，也不要基于一个看不懂的记录继续操作。

---

## 7. 验收结果

### 7.1 测试规模

| 测试 | 修正前 | 修正后 |
| --- | --- | --- |
| S1 (`ota_supervisor_test`) | 99 | **130** |
| S2 (`ota_supervisor_s2_test`) | 40 | **44** |
| 端到端 (`ota_supervisor_e2e`) | 8 组 | 8 组 |

新增的 31 项覆盖：并发写、boot_id、单次读取一致性、
立即退出、反向验证残留检测。

### 7.2 宿主机 x86-64

```text
$ make test-supervisor
检查项: 130, 失败: 0
结果: PASS

$ make test-supervisor-s2
检查项: 44, 失败: 0
结果: PASS
```

### 7.3 板端 i.MX6ULL

```text
$ adb shell /tmp/t1
检查项: 130, 失败: 0
结果: PASS

$ adb shell /tmp/t2
== 9. 启动后立即退出 ==
  PASS  启动成功（进程可能已退出）
  PASS  立即退出后 -> STOPPED（实际 STOPPED）
  PASS  无僵尸残留（实际 0 个）
  PASS  未出现长时间阻塞（耗时 0s）

检查项: 44, 失败: 0
结果: PASS
```

**板端 boot_id 可用**（内核 4.9.88），长度 36，往返一致。

### 7.4 端到端（真实 ARM Gateway，板端）

```text
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

与修正前一致（22ms），说明修正未引入性能或行为回归。

### 7.5 x86 全量回归

```text
$ make test
全部单元测试通过
```

---

## 8. 未修改 D23 代码（证明）

```sh
$ git status --short bsp/apps/ota_agent/ota_upgrade_run.c \
                       bsp/apps/ota_agent/ota_process.c \
                       bsp/apps/ota_agent/ota_health.c \
                       bsp/apps/ota_agent/ota_recover.c
（无输出）
```

---

## 9. 本次改动清单

| 文件 | 改动 |
| --- | --- |
| `ota_supervisor.h` | 新增 `OTA_SUP_BOOT_ID_MAX`、`boot_id` 字段、`ota_sup_probe_process`、`ota_sup_read_boot_id` |
| `ota_supervisor.c` | 统一 `read_stat_fields` 核心；boot_id 读写与严格校验；`reap_bounded` |
| `ota_supervisor_test.c` | 99 → 130 项；修正假阳性路径；新增并发写、boot_id、快照一致性 |
| `ota_supervisor_s2_test.c` | 40 → 44 项；新增立即退出测试 |
| `D25-2-process-lifecycle-design.md` | 修正文档状态描述 |

---

## 10. 关于复审中提到的"CI"说明

复审指出：

> 当前 GitHub commit status 没有可见的 CI 检查记录，因此上述 PASS
> 应理解为仓库验收文档记录的测试结果，而不是 GitHub Actions 自动构建通过。

**这一判断完全正确**。本项目**没有配置 CI**，
所有 PASS 均来自：

1. 本地宿主机手工执行 `make test-supervisor` / `test-supervisor-s2`；
2. 通过 `adb push` 部署到 i.MX6ULL 后手工执行。

`git push` 不会触发任何自动验证。这一点在验收文档中也应始终写明，
避免读者误以为是流水线保证。

---

## 11. 已知限制（更新）

| # | 限制 | 状态 |
| --- | --- | --- |
| L1 | `ota_process.c` 在 `-Werror` 下有既存 `unused-result` 告警 | 未处理（属 D23 代码） |
| L2 | pidfile 无文件锁，S2 只保证不产生半截内容 | 未变 |
| L3 | 未处理 pid 命名空间（容器场景） | 未变 |
| L4 | 未接入升级流程（S3） | 未变 |
| L5 | ~~启动后立即崩溃未覆盖~~ | **已覆盖**（新测试） |
| L6 | `reap_bounded` 超时后会遗留僵尸 | 新增，已知取舍 |
| L7 | boot_id 不可用时跨重启校验退化 | 新增，板端实测可用 |
| L8 | **无 CI**，所有 PASS 为手工执行结果 | 新增说明 |

---

## 12. 下一步（S3）的前置条件

复审建议"先做 S2-R 再进 S3"，本文档即为 S2-R 交付。

进入 S3 前**仍有一个未解决的阻塞**：

> D23 的集成测试 `tests/test_ota_northbound_fault.py` 依赖
> `build/ota-test/ota_baseline_init` 等手工构建的辅助二进制，
> 当前缺失，且脚本需要 `--prepare` 参数。

**S3 会首次修改 `ota_upgrade_run.c`（D23 已验证的核心流程）**，
如果没有可运行的回归测试，改动风险不可控。

因此 S3 的第一项工作应是**恢复 D23 的测试工具链**，
而不是直接改代码。
