# D25-2 S2-R2 · 第二轮复审修正

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-09
> 前置：[S1-R + S2](D25-2-S1R-S2-supervisor.md)、[S2-R](D25-2-S2R-review-fixes.md)
> **结论：复审指出的 4 个问题全部确认属实并已修正**

## 0. 结论

| # | 复审问题 | 核实 | 处理 |
| --- | --- | --- | --- |
| 1 | 并发写测试仍可能假阳性 | **属实** | 子进程统计成功次数、父进程校验退出码、测试前清理 |
| 2 | 临时文件需完整锁保护 | **属实** | 改为 pid + 原子序号；去掉危险的 EEXIST 删除重试；remove 不再扫描删临时文件 |
| 3 | 旧格式 pidfile 跨重启不安全 | **属实** | 新增 `ota_sup_identity_is_legacy()` 让调用方显式决策 |
| 4 | 立即退出测试缺外部超时 | **属实** | Makefile 加 `timeout` 包装并验证生效 |

**测试规模**：S1 130 → **145** 项；宿主机与板端 i.MX6ULL 均 PASS。

---

## 1. 问题一：并发写测试的假阳性

### 1.1 原代码的缺陷

```c
for (int r = 0; r < rounds; r++) {
    ota_sup_identity_t id = { ... };
    (void)ota_sup_pidfile_write(g_dir, &id);   // 返回值被丢弃
}
_exit(0);                                       // 不论成败都 exit(0)
```

父进程：

```c
(void)waitpid(pids[i], NULL, 0);                // 退出状态未检查
```

### 1.2 实测证明假阳性存在

构造"全部写入失败 + 预置合法 pidfile"的场景：

```text
写入全部失败，但最终检查结果: PASS（假阳性）
```

原因是测试最终只做"能读到一个合法 pidfile"这一项判断。

### 1.3 三重修正

**修正一：测试前清理**

```c
(void)ota_sup_pidfile_remove(g_dir);

ota_sup_identity_t pre;
errno = 0;
CHECK(ota_sup_pidfile_read(g_dir, &pre) == -1 && errno == ENOENT,
      "起始状态已清空（无遗留 pidfile）");
```

**修正二：子进程统计并回报成功次数**

```c
int ok_count = 0;
for (int r = 0; r < rounds; r++) {
    if (ota_sup_pidfile_write(g_dir, &id) == 0)
        ok_count++;
}
_exit(ok_count > 255 ? 255 : ok_count);   // 通过退出码回传
```

**修正三：父进程校验退出状态与成功总数**

```c
if (r != pids[i] || !WIFEXITED(status)) {
    all_exited_normally = 0;
    continue;
}
total_ok += WEXITSTATUS(status);

CHECK(all_exited_normally, "所有写者进程正常退出");
CHECK(total_ok > 0, "确有写入成功（累计 %d 次）", total_ok);
CHECK(total_ok >= writers, "成功次数不少于进程数（%d >= %d）", ...);
```

### 1.4 修正后的实际结果

```text
== 17. 并发写入不产生坏文件 ==
  PASS  起始状态已清空（无遗留 pidfile）
  PASS  所有写者进程正常退出
  PASS  确有写入成功（累计 200 次）
  PASS  成功次数不少于进程数（200 >= 4）
  PASS  并发写后 pidfile 仍可完整解析
  PASS  pid 合法（109419）
  PASS  starttime 合法（64100685）
  PASS  并发后无临时文件残留（实际 0 个）
```

**200 = 4 进程 × 50 轮，全部成功**。这个数字现在是有意义的：
它证明写入路径真的在工作，而不是被"遗留 pidfile"掩盖。

---

## 2. 问题二：临时文件与锁

### 2.1 三个独立缺陷

复审指出的问题拆开看是三个：

| 缺陷 | 后果 |
| --- | --- |
| 同一进程多线程 pid 相同 | 临时名撞车 |
| `EEXIST` 时删除重试 | 可能删掉另一个线程正在写的文件 |
| `pidfile_remove()` 扫描删除所有临时文件 | 可能删掉另一个进程正在写的文件 |

### 2.2 修正：pid + 原子序号

```c
#define OTA_SUP_TEMP_PREFIX "gateway.pid.tmp."

static unsigned long temp_seq = 0;

unsigned long seq = __sync_fetch_and_add(&temp_seq, 1);

snprintf(tmp_name, sizeof(tmp_name),
         OTA_SUP_TEMP_PREFIX "%ld.%lu", (long)getpid(), seq);
```

**为什么用 `__sync_fetch_and_add` 而不是 pthread_mutex**：

1. 不引入 pthread 依赖（本模块当前不链接 pthread）；
2. **fork 安全性**：持锁状态下 fork 会让子进程继承一把永不释放的锁。
   原子操作没有这个风险——fork 只会复制调用线程的寄存器与内存。
3. 单条指令，无锁开销。

### 2.3 去掉 EEXIST 删除重试

```c
/* 修正前 */
if (saved_errno == EEXIST) {
    (void)unlinkat(dirfd, tmp_name, 0);      // 危险：可能是别人的文件
    tmpfd = openat(... O_EXCL ...);
}

/* 修正后 */
tmpfd = openat(dirfd, tmp_name, O_WRONLY | O_CREAT | O_EXCL
               | O_CLOEXEC | O_NOFOLLOW, 0600);
if (tmpfd < 0) {
    /* 名字已唯一，正常不会 EEXIST。直接失败，不删除任何东西。 */
    return -1;
}
```

**理由**：名字已经唯一，EEXIST 只可能来自序号回绕或人为预置。
此时"删掉重试"是在赌"那个文件不是别人的"——而无法证明这一点。

### 2.4 pidfile_remove 不再扫描删临时文件

```c
/* 修正前：删除所有前缀匹配的临时文件 */
while ((entry = readdir(dir)) != NULL) {
    if (strncmp(entry->d_name, OTA_SUP_TEMP_PREFIX, ...) == 0)
        (void)unlinkat(dirfd, entry->d_name, 0);
}

/* 修正后：只删 pidfile 本体，临时文件由写入方自己负责 */
```

**误删场景**：A 正在写 `gateway.pid.tmp.100.3`，
B 调用 `remove()` 把它删了，A 的 `fsync`/`rename` 失败，
且错误原因极难理解（文件莫名消失）。

### 2.5 新增：按年龄清理孤儿文件

崩溃可能留下临时文件。新增独立接口：

```c
int ota_sup_cleanup_stale_temp(
    const char *install_dir,
    int older_than_seconds);
```

**用"文件年龄"而不是"是否属于当前进程"来判断**：
崩溃遗留的文件可能来自任意 pid，而活跃写者的文件总是很新。
写一个 pidfile 是毫秒级操作，60 秒前的文件必然是孤儿。

测试验证两个方向：

```text
PASS  只删除 1 个陈旧临时文件（实际 1）
PASS  陈旧临时文件已删除
PASS  新鲜临时文件被保留（不误删活跃写者）
```

### 2.6 关于锁：S3 的强制要求

本模块提供的隔离只解决**文件名冲突**，
**不提供单写者语义**。pidfile 的语义是"唯一一份记录"，
多个写者并发更新仍会产生"后写者胜"。

**必须由调用方持有 `.ota.lock`** 串行化整个
「查询 → 启动 → 登记 → 停止 → 删除」流程。

这一条已写入 S3 的准入条件（见第 5 节）。

---

## 3. 问题三：旧格式 pidfile 的跨重启安全性

### 3.1 风险

旧格式 `12345 678\n` 不含 boot_id。重启后：

- pid 从 1 重新分配；
- jiffies 从 0 重新计。

一条来自上次启动的记录，**完全可能**在新周期中碰巧匹配到无关进程。

### 3.2 为什么不直接拒绝旧格式

直接拒绝会导致：升级后遗留的旧 pidfile 被判损坏 →
`query()` 返回 UNKNOWN → 启动被 `EBUSY` **永久拒绝**。
系统会卡死在这里，需要人工删文件才能恢复。

### 3.3 修正：让调用方显式决策

```c
int ota_sup_identity_is_legacy(
    const char *install_dir,
    int *is_legacy_out);

/* 返回 0：新格式（含 boot_id），可安全校验
   返回 1：旧格式（无 boot_id），跨重启安全性弱
   返回 -1：读不到或格式损坏 */
```

**本模块不替调用方做决定，只如实报告**。典型策略：

| 场景 | 建议策略 |
| --- | --- |
| 全新部署 | 不应出现旧格式；出现则视为可疑 |
| 从旧版本升级 | 允许一次，成功后立即重写为新格式 |
| 来源不明 | 保守：视为 UNKNOWN，要求显式清理 |

测试：

```text
PASS  旧格式可识别
PASS  识别为旧格式（实际 1）
PASS  新格式可识别
PASS  识别为新格式（实际 0）
PASS  无记录时返回 -1/ENOENT
```

---

## 4. 问题四：测试缺外部超时

### 4.1 缺陷

```make
test-supervisor-s2:
	$(CC) ... -o $(HOST_S2_BIN)
	@$(HOST_S2_BIN)          # 无 timeout
```

`elapsed_s <= 5` 是**程序返回后**才执行的断言。
若测试自身挂起（fork 的子进程不退出、父进程无界 `waitpid`），
根本执行不到那行断言。

### 4.2 修正

```make
TEST_TIMEOUT ?= 120

test-supervisor-s2:
	$(CC) ... -o $(HOST_S2_BIN)
	@timeout $(TEST_TIMEOUT) $(HOST_S2_BIN) || \
		{ rc=$$?; [ $$rc -eq 124 ] && echo "FAIL: 测试超时 ($(TEST_TIMEOUT)s)"; exit $$rc; }
```

### 4.3 实测验证超时确实生效

不能只看"测试通过"就断言超时可用——正常测试根本跑不满 120 秒。
我用一个必然挂起的程序验证包装逻辑：

```text
$ make -f mk.mk t          # 目标为 for(;;) pause();
FAIL: 测试超时 (3s)
make: *** [mk.mk:4: t] Error 124
make exit=2
```

**结论**：超时能触发、能打印明确信息、能以非零码退出。

同时确认 `timeout` 本身的退出码语义正确：

```text
$ timeout 5 ./hang
exit=124      # 124 = 被 timeout 终止
```

---

## 5. S3 的准入条件（必须满足）

基于本次修正，S3 接入时必须做到：

| # | 条件 | 理由 |
| --- | --- | --- |
| A1 | 所有 supervisor 操作包在 `.ota.lock` 排他锁内 | 本模块只解决文件名冲突，不提供单写者语义 |
| A2 | 升级流程开头检查 `ota_sup_identity_is_legacy()` | 旧记录跨重启不安全，需显式策略 |
| A3 | 崩溃恢复路径调用 `ota_sup_cleanup_stale_temp(dir, 60)` | 清理孤儿临时文件，且不误伤活跃写者 |
| A4 | **先恢复 D23 测试链路** | 见下 |

### A4 说明

D23 的集成测试 `tests/test_ota_northbound_fault.py`：

- 依赖 `build/ota-test/ota_baseline_init` 等**手工构建**的辅助二进制，当前缺失；
- 需要 `--prepare` 参数才能初始化安装目录。

S3 会**首次修改 `ota_upgrade_run.c`**（D23 已验证的核心流程）。
没有可运行的回归测试就改核心流程，风险不可控。

**因此 S3 的第一项工作应是恢复这批测试工具，而不是直接改代码。**

---

## 6. 验收结果

### 6.1 测试规模

| 测试 | S2-R | S2-R2 |
| --- | --- | --- |
| S1 | 130 | **145** |
| S2 | 44 | 44 |
| 端到端 | 8 组 | 8 组 |

新增 15 项：临时文件唯一性（20 次连续写）、
孤儿清理（新鲜保留 / 陈旧删除）、旧格式识别（3 种输入）。

### 6.2 宿主机 x86-64

```text
$ make test-supervisor
检查项: 145, 失败: 0
结果: PASS

$ make test-supervisor-s2
检查项: 44, 失败: 0
结果: PASS
```

### 6.3 板端 i.MX6ULL

```text
== 17. 并发写入不产生坏文件 ==
  PASS  起始状态已清空（无遗留 pidfile）
  PASS  所有写者进程正常退出
  PASS  确有写入成功（累计 200 次）
  PASS  成功次数不少于进程数（200 >= 4）
  ...
检查项: 145, 失败: 0
结果: PASS

$ /tmp/t2
检查项: 44, 失败: 0
结果: PASS
```

**板端 200 次并发写同样全部成功**，说明 `__sync_fetch_and_add`
在 ARMv7 上的行为正确（单核 Cortex-A7，原子指令由内核辅助实现）。

### 6.4 端到端（真实 ARM Gateway）

```text
  PASS  退出码为 0
== 5. 停止后状态 ==
  PASS  query -> STOPPED
  PASS  pidfile 已清理
  PASS  无僵尸残留（waitpid=ECHILD）
  PASS  /proc 条目已消失
结果: PASS
```

### 6.5 x86 全量回归

```text
$ make test
全部单元测试通过
```

---

## 7. 未修改 D23 代码

```sh
$ git status --short bsp/apps/ota_agent/ota_upgrade_run.c \
                       bsp/apps/ota_agent/ota_process.c \
                       bsp/apps/ota_agent/ota_health.c \
                       bsp/apps/ota_agent/ota_recover.c
（无输出）
```

---

## 8. 本次改动清单

| 文件 | 改动 |
| --- | --- |
| `ota_supervisor.h` | 新增 `ota_sup_cleanup_stale_temp`、`ota_sup_identity_is_legacy`；修正 `remove` 的语义说明 |
| `ota_supervisor.c` | 临时名加原子序号；去掉 EEXIST 删除重试；`remove` 不再扫描；新增孤儿清理与旧格式识别 |
| `ota_supervisor_test.c` | 130 → 145 项；并发测试改为校验返回值与退出码；新增唯一性/清理/旧格式测试 |
| `Makefile` | 加入 `TEST_TIMEOUT` 外部超时包装 |

---

## 9. 已知限制（更新）

| # | 限制 | 状态 |
| --- | --- | --- |
| L1 | `ota_process.c` 的 `unused-result` 告警 | 未处理（D23 代码） |
| L2 | ~~pidfile 无锁~~ | **明确为 S3 准入条件 A1**，本模块不提供 |
| L3 | pid 命名空间（容器） | 未处理 |
| L4 | 未接入升级流程 | S3 |
| L5 | ~~立即退出未覆盖~~ | 已覆盖 |
| L6 | `reap_bounded` 超时遗留僵尸 | 已知取舍 |
| L7 | boot_id 不可用时降级 | 板端实测可用 |
| L8 | 无 CI，PASS 均为手工执行 | 不变 |
| L9 | **旧格式 pidfile 跨重启不安全** | 新增 `is_legacy` 供调用方决策，S3 须处理（A2） |
| L10 | 同一 `.ota.lock` 之外的多写者仍会"后写者胜" | S3 须加锁（A1） |

---

## 10. 后续

S2-R2 完成。按第 5 节的 A1–A4，**S3 的第一步是恢复 D23 测试工具链**，
而不是直接修改 `ota_upgrade_run.c`。

D23 测试的依赖清单需要在 S3 开始时重新梳理，
当前已知至少缺 `build/ota-test/ota_baseline_init`。
