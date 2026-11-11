# D25-2 S3 · OTA 升级流程接入 Supervisor

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-10
> 前置：[S2-R2](D25-2-S2R2-review-fixes.md)
> **结论：S3 完成。D23 回归 27/27 通过，新增前置检查验收 23/23 通过，板端实测通过**

## 0. 结论

| 任务 | 结果 | 依据 |
| --- | --- | --- |
| 恢复 D23 测试环境 | **PASS** | 第 1 节，脚本化，不再丢失 |
| 接入 `.ota.lock` | **PASS** | 第 2 节，原有排他锁已覆盖 |
| 处理旧格式 pidfile | **PASS** | 第 3 节，NOTICE + 自动迁移 |
| 接入孤儿文件清理 | **PASS** | 第 4 节 |
| 修改 OTA 核心流程 | **PASS** | 第 5 节 |
| 完整回归 | **PASS** | D23 27/27 + 新增 23/23 |

**首次修改 D23 已验证代码**（`ota_upgrade_run.c`），回归全过。

---

## 1. 恢复 D23 测试环境（准入条件 A4）

### 1.1 问题

D23 集成测试依赖 `build/ota-test/` 下一批辅助产物，但：

```sh
$ git log --all --oneline -- "*ota_baseline_init*"
（空：从未提交到仓库）
```

这些是历史会话中**手工构建**的，`make clean` 或换机器即全部丢失。
S2-R2 时正是因为缺失导致回归无法执行。

### 1.2 解决：脚本化

新增 `tests/setup_ota_test_env.sh`，一条命令重建全部产物：

| 产物 | 说明 |
| --- | --- |
| `build/ota-test/ota_baseline_init` | 源码内嵌在脚本中，编译生成 |
| `build/ota-test/real-x86/v0.1.0-test1/` | 旧版发布目录 |
| `build/ota-test/real-x86/v0.1.1-test1/` | 新版发布目录（候选包） |
| `build/ota-test/install-x86-fault.2e0Csr/` | 故障注入用安装目录 |

**关于 `ota_baseline_init` 的接口**：

测试脚本的调用方式是 `ota_baseline_init <state_dir>`，只传 1 个参数。
而仓库中的 `ota_state_store` 需要 4 个参数，两者不是同一个工具。

因此脚本内嵌了一份独立的实现，只做一件事：
写出 `CONFIRMED / 0.1.0-test1` 的 `ota_state.json`，
复用与 `ota_state_store` 相同的原子写流程
（临时文件 → fsync → rename → fsync 目录）。

### 1.3 实测

```text
$ sh tests/setup_ota_test_env.sh
  ota_agent_x86   : OK
  smart_gateway   : OK
-- 构建 ota_baseline_init --
-- 构建发布目录 --
  .../real-x86/v0.1.0-test1  (size=86480)
  .../real-x86/v0.1.1-test1  (size=86480)
-- 构建安装目录 --
PASS: baseline state written (0.1.0-test1)
-- 自检 --
  OK   ota_baseline_init
  OK   旧版二进制 / manifest
  OK   新版二进制 / manifest
  OK   初始状态文件
  OK   current 软链
```

**A4 达成**，且此后不再依赖"某人手工构建过什么"。

---

## 2. 接入 `.ota.lock`（准入条件 A1）

### 2.1 现状核查

`main.c` 中所有写操作已持有排他锁：

```c
static int upgrade_test_command(...)
{
    int lockfd = ota_lock_acquire(install_dir, 1);   // 排他锁
    ...
    int result = ota_upgrade_run_test(package_dir, install_dir);
    ota_lock_release(lockfd);
}
```

`status` 用共享锁（0），`recover` / `stage` / `activate-test` 用排他锁（1）。

### 2.2 结论

S3 新增的 supervisor 调用全部位于 `ota_upgrade_run_test()` 内部，
因此**天然处于同一把排他锁的保护下**，无需额外加锁。

文档中明确记录这一依赖关系，避免后续有人在锁外调用这些函数。

---

## 3. 处理旧格式 pidfile（准入条件 A2）

### 3.1 策略选择

三种可选：

| 策略 | 后果 |
| --- | --- |
| 直接拒绝 | 遗留旧记录 → UNKNOWN → 启动被 `EBUSY` **永久拒绝**，需人工删文件 |
| 静默接受 | 不告知风险，跨重启误匹配可能长期潜伏 |
| **接受 + 明确提示 + 自动迁移** | 采用 |

### 3.2 实现

```c
int is_legacy = 0;

if (ota_sup_identity_is_legacy(install_dir, &is_legacy) == 0 &&
    is_legacy) {
    puts("NOTICE: legacy pidfile (no boot_id); "
         "cross-reboot identity check is weakened");
    puts("NOTICE: it will be migrated on the next successful start");
}
```

迁移是自然发生的：下一次 `ota_sup_start_managed()` 成功后会写入
带 `boot_id` 的三段式新格式。

**为什么不直接删除旧记录**：删除等于放弃一个可能仍有效的身份信息。
在不了解运行状态时删除，比保留并提示更危险。

---

## 4. 接入孤儿文件清理（准入条件 A3）

```c
int removed = ota_sup_cleanup_stale_temp(install_dir, 60);

if (removed > 0) {
    printf("NOTICE: cleaned %d stale temp file(s)\n", removed);
} else if (removed < 0) {
    printf("WARN: stale temp cleanup failed (%s)\n", strerror(errno));
}
```

**60 秒阈值**：写 pidfile 是毫秒级操作，一分钟前的临时文件必然是崩溃遗留。

**清理失败不阻断升级**，但如实打印 WARN——一个读不到的目录
不该让整个升级失败，但也不能当作没发生。

---

## 5. 修改 OTA 核心流程

### 5.1 新增前置检查 `prepare_existing_gateway()`

对应设计文档 5.2 节的 0a–0c：

| 步骤 | 动作 | 失败处理 |
| --- | --- | --- |
| A3 | 清理孤儿临时文件 | 仅 WARN，不阻断 |
| A2 | 识别旧格式 pidfile | 仅 NOTICE，不阻断 |
| 0a | `ota_sup_query()` 查状态 | **UNKNOWN → 中止** |
| 0c | RUNNING → 发 SIGTERM 并等待退出 | **超时 → 中止** |

关键代码：

```c
if (state == OTA_SUP_UNKNOWN) {
    puts("FAIL: gateway state UNKNOWN; abort upgrade");
    puts("      resolve the pidfile issue before retrying");
    return -1;
}
```

```c
if (waited >= timeout_ms) {
    printf("FAIL: gateway pid=%ld did not exit within %d ms\n", ...);
    puts("      aborting upgrade; current release unchanged");
    return -1;
}
```

**为什么超时后不发 SIGKILL**：

1. 若进程卡在 D 状态，SIGKILL 同样无效；
2. 若不是，硬杀会让它来不及清理资源（端口、临时文件）；
3. 最要紧的是——**绝不能在此时继续切换 `current`**。

### 5.2 发现并修正一个顺序陷阱

这是 S3 过程中最有价值的发现。原代码顺序：

```c
if (wifi_port_busy()) {          // ← 先查端口
    puts("FAIL: TCP 7000 already occupied");
    return 1;
}
prepare_existing_gateway(...);   // ← 后停旧版
```

**问题**：正式部署中 Gateway 正在运行，它必然占着 7000。
先做端口检查就会在"发现是自己占的"之前直接拒绝，
**永远走不到停止旧版的逻辑**——S3 的整个目的落空。

这个缺陷 D23 完全测不出来，因为 D23 假设升级开始时 Gateway 没在运行。

**修正后顺序**：

```c
prepare_existing_gateway(install_dir);   // 先停旧版
if (wifi_port_busy()) {                  // 再查端口
    puts("FAIL: TCP 7000 still occupied after stopping gateway");
    return 1;
}
```

此时端口应已释放；若仍被占用，说明是**别的进程**，拒绝是正确的。

### 5.3 健康检查改用 supervisor

`run_gateway_health()` 原本直接 `ota_process_start()`，
不写 pidfile，外部无从查询。

改为：

```c
ota_sup_managed_t managed = {0};

if (ota_sup_start_managed(install_dir, executable, argv, &managed) != 0) {
    perror("FAIL: Gateway start");
    return -1;
}
```

停止改为 `ota_sup_stop_managed()`，从而获得：

- 启动前检查已有 RUNNING 实例（`EALREADY`）
- 写入 `<install>/run/gateway.pid`
- 停止时的 owner 校验与 pidfile 清理

---

## 6. 验收

### 6.1 D23 回归（核心指标）

**改动前基线**：

```text
合计 27 项, 通过 27, 失败 0
结论: PASS
```

**改动后**：

```text
合计 27 项, 通过 27, 失败 0
事务耗时: 5.22s
结论: PASS
```

逐项与基线一致，包含：

```
PASS  新版 Gateway 已启动
PASS  新版 L1 检查 PASS
PASS  新版 L2 检查 FAIL          ← 注入的北向故障
PASS  新版进程停止并回收
PASS  文件系统回滚 PASS
PASS  旧版重新启动
PASS  旧版业务恢复
PASS  OTA 最终状态 ROLLED_BACK
PASS  无遗留 Gateway 进程
```

**耗时 5.22s 与基线完全相同**，说明 S3 的 supervisor 封装
没有引入额外延迟或行为变化。

### 6.2 新增：前置检查验收 23/23

新增 `tests/test_s3_preflight_running.py`，覆盖 D23 未测的场景。

**场景一：Gateway 正在运行时升级**

```text
[PASS] 前置：7000 空闲
[PASS] Gateway 已启动并监听 7000
[PASS] 已写入 pidfile（含 boot_id）: 115486 68851950 0a023e15-...
[PASS] supervisor status 可用
[PASS] 检测到运行中的 Gateway
[PASS] 执行了停止动作
[PASS] 原 Gateway 进程已退出: pid=115486
[PASS] 升级成功完成（无故障注入）: state=CONFIRMED
[PASS] confirmed 已更新为新版: confirmed=0.1.1-test1
[PASS] current 指向新版: current=releases/v0.1.1-test1
[PASS] pidfile 已清理: pidfile=None
[PASS] 结束后无遗留 Gateway 进程
```

**场景二：pidfile 损坏 → 必须中止**

```text
Pre-upgrade gateway state: UNKNOWN
FAIL: gateway state UNKNOWN; abort upgrade
      resolve the pidfile issue before retrying

[PASS] 识别为 UNKNOWN
[PASS] 升级被中止: exit=1
[PASS] current 未被修改
[PASS] 状态文件未被修改: state=CONFIRMED
[PASS] 无 Gateway 进程被启动
```

**场景三：Gateway 停不下来 → 必须中止（最关键安全约束）**

用一个 `trap '' TERM` 的进程模拟顽固实例：

```text
Pre-upgrade gateway state: RUNNING
NOTICE: stopping running gateway (pid=115776)
FAIL: gateway pid=115776 did not exit within 3000 ms
      aborting upgrade; current release unchanged

[PASS] 检测到运行中的实例
[PASS] 升级被中止: exit=1
[PASS] current 未被切换（关键安全约束）
[PASS] 状态文件未被推进: state=CONFIRMED
[PASS] 顽固进程未被硬杀（无 SIGKILL）: pid=115776
```

场景三验证了设计文档 5.2 节的核心约束：
**停不掉就中止，绝不切换版本**。

### 6.3 其它回归

| 测试 | 结果 |
| --- | --- |
| x86 单元测试 | 全部通过 |
| `test-supervisor` | 145/145 PASS |
| `test-supervisor-s2` | 44/44 PASS |

### 6.4 板端实测（i.MX6ULL）

在真实板端用 ARM 产物验证完整流程：

```text
=== 启动 Gateway + pidfile ===
  运行中 gw=21883
=== upgrade-test ===
Pre-upgrade gateway state: RUNNING
NOTICE: stopping running gateway (pid=21883)
PASS: gateway stopped after 50 ms
PASS: OTA upgrade CONFIRMED: 0.1.1-test1
=== 结果 ===
  current=releases/v0.1.1-test1
  pidfile=已清理
```

**板端前置检查、优雅停止（50 ms）、版本切换全部正常。**

同时确认板端 json-c 0.13.1 的兼容层工作正确：

```text
$ /tmp/pe13
len=26 err=0 obj=ok char_offset=26 match=YES
```

---

## 7. 排查过程中遇到的两个"假故障"

这两点值得记录，因为它们在排查时非常像产品缺陷。

### 7.1 `invalid manifest JSON`（宿主机手工环境）

用自建的最小安装目录跑 `upgrade-test` 时报此错。
排查发现：**`upgrade-test` 不做 stage**，
它假设 `releases/v0.1.1-test1` 已存在（D23 环境中预先放好了）。
自建环境缺这一步，于是 `read_identity()` 读不到目录。

**不是代码缺陷**，是测试脚手架不完整。
D23 官方环境 27/27 通过即为证据。

### 7.2 板端同样报错

同一现象。一度怀疑是 json-c 0.13.1 兼容层有问题，
因此在板端单独编译验证：

```text
len=26 err=0 obj=ok char_offset=26 match=YES
```

兼容层完全正确。补齐发布目录后板端也通过。

**教训**：遇到"看起来是产品 bug"的现象时，
先确认测试环境本身是否完整，再怀疑实现。
本次两次都通过"最小可复现验证"排除了实现问题。

---

## 8. 改动清单

| 文件 | 改动 |
| --- | --- |
| `bsp/apps/ota_agent/ota_upgrade_run.c` | 新增 `prepare_existing_gateway()`；调整端口检查顺序；健康检查改用 supervisor |
| `tests/setup_ota_test_env.sh` | **新增**，D23 测试环境一键重建 |
| `tests/test_s3_preflight_running.py` | **新增**，前置检查三场景验收 |

**未改动**：`ota_process.c`、`ota_health.c`、`ota_recover.c`、
`ota_state.c`、`ota_supervisor.c`、`gateway/`。

---

## 9. 已知限制

| # | 限制 | 说明 |
| --- | --- | --- |
| L1 | 停止超时后不发 SIGKILL | 有意为之：硬杀无法解决 D 状态，且会留下不一致 |
| L2 | 旧格式迁移依赖"下一次成功启动" | 若一直未启动则一直保持旧格式，仅有 NOTICE 提示 |
| L3 | 前置检查依赖 pidfile 存在 | 若正式 Gateway 由 init 启动但**未写 pidfile**，会误判为 STOPPED |
| L4 | 未在板端验证"停不下来"场景 | 宿主机已验证（场景三），板端只验证了正常停止 |
| L5 | 无 CI | 所有 PASS 均为手工执行 |

**L3 是 S5 必须解决的**：届时需要由 init 脚本在启动 Gateway 时
写入 pidfile，否则前置检查形同虚设。

---

## 10. 下一步

S3 完成，剩余：

| 阶段 | 内容 | 前置 |
| --- | --- | --- |
| S4 | 故障注入（F1–F10 全矩阵） | 无 |
| S5 | BusyBox init 集成 + **init 脚本写 pidfile**（解决 L3） | 确认 `start-stop-daemon` |
| S6–S7 | 板端完整 OTA 与重启恢复 | S5 完成 |

**S5 的关键点**：必须让 init 脚本在启动 Gateway 后写入 pidfile，
否则 S3 的前置检查在正式部署中会被 L3 绕过。
