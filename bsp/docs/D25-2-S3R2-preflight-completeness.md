# D25-2 S3-R2 · 前置校验补全

> 项目：SmartGateway-BSP / 100ASK i.MX6ULL
> 分支：`feature/bsp-imx6ull`
> 记录日期：2026-11-10
> 前置：[S3-R 复审修正](D25-2-S3R-review-fixes.md)
> **结论：复审指出的校验遗漏属实并已修复。前置校验从"只验包"扩展为"验完 activate 的全部只读条件"**

## 0. 结论

| 校验项 | S3-R 是否前置 | S3-R2 |
| --- | --- | --- |
| 升级包 SHA-256 | 是 | 是 |
| ELF 架构 | 是 | 是 |
| `releases` 目录存在 | 是 | 是 |
| **候选版本 ≠ 已确认版本** | **否** | **是** |
| **已确认版本文件完整性** | **否** | **是** |
| **`current` 指向已确认版本** | **否** | **是** |
| **候选 release 已安装** | **否** | **是** |
| **已安装候选与包 SHA-256 一致** | **否** | **是** |

测试规模：S3 前置验收 31 → **40** 项。

---

## 1. 问题：前置校验只覆盖了一部分

### 1.1 复审指出的场景

升级包本身完全合法，但 `releases/v0.1.1-test1/` 并不存在（未 stage）。

S3-R 的 `preflight_package()` 只检查了 `releases` **目录**存在，
没检查 `releases/<candidate>` 是否存在，于是：

1. precheck 通过
2. `prepare_existing_gateway()` 停掉旧 Gateway
3. `ota_activate_test()` 才发现候选版本缺失
4. 升级失败，旧 Gateway 已经停了

**又是一次本可避免的业务中断**——S3-R 修了"包损坏"，但没覆盖这类"包合法、环境不满足"的情形。

### 1.2 逐个对照

把 `ota_activate_test()` 的校验逐条与 `preflight_package()` 对齐：

| `ota_activate_test()` 中的检查 | S3-R 前置 |
| --- | --- |
| `ota_verify_package(package_dir)` | ✅ |
| `read_identity(package_dir, &source)` | ✅（通过 verify 间接） |
| 状态为 `CONFIRMED` / `ROLLED_BACK` | ❌ |
| `current` == `releases/v<confirmed>` | ❌ |
| `source.version != confirmed_version` | ❌ |
| `ota_verify_package(old_path)` | ❌ |
| `confirmed.version == record.confirmed_version` | ❌ |
| `ota_verify_package(new_path)` | ❌ |
| `installed.version == source.version` | ❌ |
| `installed.sha256 == source.sha256` | ❌ |

复审列出的 5 项遗漏**全部属实**，另外还有 2 项（状态检查、current 一致性）同样未前置。

---

## 2. 修复：把 activate 的只读校验抽成独立阶段

### 2.1 关键设计决定：**共用同一份实现**

最直接的做法是在 `preflight_package()` 里把那些检查**再写一遍**。
我没有这样做，理由：

> 如果 precheck 和 activate 各写一份校验，两者会随时间分叉。
> 某天有人给 activate 加了一条新检查，忘了同步到 precheck，
> "预检通过 → 停业务 → 激活被拒"的问题就会重新出现。

因此改为：把校验抽成 `activate_check()`，两处都调用它。

```c
/* ota_upgrade.c */

static int activate_check(
    const char *package_dir,
    const char *install_dir,
    package_identity_t *source_out)      /* 输出源包身份，避免重复解析 */
{
    /* 全部只读：不写状态、不切 current、不建删文件 */
    ...检查 1..6...
}

int ota_activate_precheck(               /* 对外只读入口 */
    const char *package_dir,
    const char *install_dir)
{
    return activate_check(package_dir, install_dir, NULL);
}

int ota_activate_test(                   /* 变更入口 */
    const char *package_dir,
    const char *install_dir)
{
    /* 第一步：与 precheck 完全相同的只读校验 */
    if (activate_check(package_dir, install_dir, &source) != 0)
        goto out;

    /* 第二步起才是变更：写状态、切 current */
    ota_state_begin_upgrade(...);
    advance_state(..., OTA_VERIFIED);
    advance_state(..., OTA_STAGED);
    activate_current(...);
    advance_state(..., OTA_ACTIVATED);
    ...
}
```

**收益**：即使将来有人给 activate 加新检查，precheck 会自动同步，
因为它们是同一个函数。

### 2.2 六项检查的清单

`activate_check()` 覆盖：

| # | 检查 | 失败信息 |
| --- | --- | --- |
| 1 | 无进行中的事务 | `OTA transaction already active` |
| 2 | `current` 指向已确认版本 | `confirmed/current mismatch` |
| 3 | 升级包合法 | （verify 内部信息） |
| 4 | 候选 ≠ 已确认 | `candidate equals confirmed` |
| 5 | 已确认版本完整且自洽 | `confirmed release invalid` |
| 6 | **候选已安装且与包 SHA-256 一致** | `installed candidate mismatch` |

第 6 条正是复审指出的核心遗漏。

### 2.3 调用方调整

`ota_upgrade_run.c` 的 `preflight_package()` 现在直接调用
`ota_activate_precheck()`：

```c
static int preflight_package(
    const char *package_dir,
    const char *install_dir)
{
    if (ota_activate_precheck(package_dir, install_dir) != 0) {
        puts("FAIL: upgrade precheck failed");
        puts("      upgrade aborted before stopping the gateway");
        return -1;
    }

    puts("PASS: preflight checks done; safe to stop the gateway");
    return 0;
}
```

原先手写的"检查 releases 目录存在"被删除了——它已被第 6 项完整覆盖
（候选要么存在且合法，要么 `installed candidate mismatch`）。

---

## 3. 验收

### 3.1 复现：修复前会中断业务

构造"合法包 + 候选未 stage"：

```text
Gateway: pid=... 7000=True
候选版本目录 v0.1.1-test1 存在: False

--- 用合法包但候选未安装 ---
```

修复前后的差别在于**失败发生的位置**，而非是否失败。
S3-R 版本会先停掉 Gateway，S3-R2 版本在停止之前就拒绝。

### 3.2 修复后实测（宿主机）

```text
PASS: OTA package verification successful
Application : smart_gateway
Version     : 0.1.1-test1
...
PASS: OTA package verification successful
Application : smart_gateway
Version     : 0.1.0-test1
...
FAIL: installed candidate mismatch          ← 第 6 项检查命中
FAIL: upgrade precheck failed
      upgrade aborted before stopping the gateway

=== 结果 ===
退出码: 1
原 Gateway 仍在运行: True
7000 仍监听: True
```

### 3.3 固化为场景五

新增 `scenario_candidate_missing`：

```text
[PASS] 候选版本目录已删除（模拟未 stage）
[PASS] Gateway 已启动并监听 7000
[PASS] 升级包本身合法
FAIL: installed candidate mismatch
FAIL: upgrade precheck failed
      upgrade aborted before stopping the gateway
[PASS] 升级被拒绝: exit=1
[PASS] 在停止 Gateway 之前就拒绝了
[PASS] 原 Gateway 仍在运行（业务未中断）
[PASS] 7000 仍在监听
[PASS] current 未变: current=releases/v0.1.0-test1
[PASS] 状态未变: state=CONFIRMED
```

### 3.4 板端验证（i.MX6ULL）

```text
gw=8196  候选目录v0.1.1-test1: 不存在
=== 合法包但未 stage ===
FAIL: installed candidate mismatch
FAIL: upgrade precheck failed
      upgrade aborted before stopping the gateway
=== 关键检查 ===
  OK 原 Gateway 仍在运行
  OK 7000 仍监听
  current=releases/v0.1.0-test1
```

### 3.5 完整回归

| 测试 | 结果 |
| --- | --- |
| D23 回归 | **27/27** PASS，耗时 5.23s |
| S3 前置验收 | **40/40** PASS（原 31 + 场景五 9 项） |
| x86 单元测试 | 全部通过 |
| `test-supervisor` | 145/145 PASS |
| `test-supervisor-s2` | 44/44 PASS |

D23 耗时 5.23s，与 S3-R 的 5.18s 基本一致，说明把校验做两遍（precheck 一次、
activate 一次）的开销可以忽略——校验本身是读文件 + 算 SHA-256，
对 86KB 的二进制来说是毫秒级。

---

## 4. 改动清单

| 文件 | 改动 |
| --- | --- |
| `bsp/apps/ota_agent/ota_upgrade.c` | 抽出 `activate_check()`；新增 `ota_activate_precheck()`；`ota_activate_test()` 改为先调同一校验再进入变更阶段 |
| `bsp/apps/ota_agent/ota_upgrade.h` | 新增 `ota_activate_precheck()` 声明与语义说明 |
| `bsp/apps/ota_agent/ota_upgrade_run.c` | `preflight_package()` 改为调用 `ota_activate_precheck()` |
| `tests/test_s3_preflight_running.py` | 新增场景五；抽出 `_start_gateway()` 辅助函数 |

---

## 5. 关于"校验做两遍"的说明

现在每次升级会执行两次完整校验：

1. `preflight_package()` → `ota_activate_precheck()`，在停止 Gateway **之前**
2. `ota_activate_test()` → `activate_check()`，在变更阶段开头

**这不是冗余，是必要的**：

- 第一次回答"值不值得停业务"
- 第二次回答"现在能否真的改"

两次之间会经历"停止 Gateway"这一步，环境可能已经变化
（例如有人手动删了候选目录）。第二次校验是变更前的最后一道闸门。

不复用第一次的结果，是因为**状态可能真的变了**——这正是
"预检通过但激活失败"应该被允许发生的情形，只是此时
不应再造成业务中断（因为业务已经停了，这时应该明确回滚）。

---

## 6. 已知限制（不变）

| # | 限制 | 状态 |
| --- | --- | --- |
| L1 | 停止超时后不发 SIGKILL | 不变 |
| L2 | 旧格式迁移依赖下一次成功启动 | 不变 |
| L3 | 前置检查依赖 pidfile 存在 | 不变，**S5 必须解决** |
| L4 | 未在板端验证"停不下来"场景 | 不变 |
| L5 | 无 CI | 不变 |
| L6 | 外部进程停止存在 TOCTOU 窗口 | 不变，**S5 应改由 init 停止** |

**L3 与 L6 都指向 S5**：由 init 统一管理进程生命周期，
既能保证 pidfile 被写入，又能从架构上移走 TOCTOU 风险。

---

## 7. 一个可复用的判断标准

这次复审暴露的模式值得记下来：

> **"前置校验"的完整性标准不是"我检查了什么"，而是
> "变更阶段还会检查什么"。**

只要变更阶段有一条校验没被前置，就存在"停掉业务后才发现不能升级"的可能。

实现上的落地方式：**让前置校验和变更阶段共用同一个校验函数**，
而不是靠人工逐条对照两份代码。人工对照会漏，共用实现不会。
