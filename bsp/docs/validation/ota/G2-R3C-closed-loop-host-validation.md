# G2-R3C 主机端闭环与安全整改复验报告

- **日期：** 2026-11-17
- **分支：** `feature/imx6ull-system-ab-ota`
- **主机侧评审状态：** **`READY-FOR-INDEPENDENT-REVIEW`（待独立安全复审确认后再正式签署 `HOST-ACCEPTED`；未推送远端、未上板）**
- **板端真机状态：** **`BOARD-PENDING / BLOCKED`**（本轮未触碰开发板 `/dev/mmcblk1`、真实 U-Boot 环境或 Watchdog）

---

## 1. 安全审查整改项（相对 `1724a0e` 新增硬化）

| 审查点 | 问题/风险 | 整改实现 | 回归验证 |
|---|---|---|---|
| **1. `--running` 检查时序** | 旧版 `sgw_bootctl confirm --running` 在调用 `r3c_coordinator_confirm()` 写入并提交 `fw_env` 之后才检查 `--running` 槽位一致性 | 将 `expected_running_slot` 传入 `r3c_coordinator_confirm()`，在 `ab_detect_running_slot_full()` 之后、**任何 `CONFIRMING` 写盘或 `fw_env_backend_save()` 之前**核验并拒绝不匹配槽位 | `test_n10_caller_running_mismatch_rejects_before_any_mutation`：注入真实 `DATA` 健康转发下传错误 `--running 2`，证实拒绝且 `fw_env` 仍为 `trial`、`txn` 仍为 `PENDING (consumed=0)` |
| **2. 分裂状态崩溃恢复与抗重放** | `fw_env_backend_save` 已提交 `confirmed` 但紧随其后的 `r3c_txn_finish_confirm` 失败或掉电时，会遗留 `env=confirmed` 与 `txn=CONFIRMING` 的孤儿状态 | 新增 `r3c_txn_reconcile_confirm()` 与协调器分裂恢复分支：持双锁并重验 DATA 持久性及安装槽镜像 SHA-256 后，将 `CONFIRMING` 原子落盘并 `fsync` 为 `CONFIRMED (consumed=1)`；任何再次调用均因无未消费事务被严格拒绝 | `test_n10_split_state_env_confirmed_txn_confirming_recovery_and_idempotency`：精确拦截第 2 次 `.txn-` `fsync` 制造分裂现场后，验证跨 `boot_id` 重启恢复、`tamper` 阻断恢复、恢复后重复调用拒绝 |
| **3. 普通调用者与 CLI/Env 权限边界** | 生产 `sgw_bootctl confirm` 需对普通用户及 `-c`/`--cmdline`/`SGW_*` 环境变量覆盖免疫 | `sgw_bootctl confirm` 入口显式检查 `geteuid() == 0`、拒绝任何 `-c`/`--config`/`--run-dir`/`--cmdline`/`--mounts`/`--sysroot` 参数，强制硬编码 `/usr/sbin/fw_*` 与 `/proc/*` 并忽略 `SGW_*`；`r3c_coordinator_confirm` 在 `ops==NULL` 时同样校验 `geteuid()==0 && trusted_uid==0` | `test_n10_unprivileged_production_sgw_bootctl_and_override_rejected` + `test_g2_uboot_ab`：`ubuntu` 非特权调用及带覆盖参数调用均输出 `CONFIRM REJECTED` 且环境零改动 |
| **4. DATA 身份、锁范围与健康证据时效** | 需防止设备路径父目录篡改、策略自相矛盾、慢探针伪造样本时间戳 | `r3c_data_inspect_full()` 增加 `trusted_dirs(p->device)`；`r3c_policy_load()` 禁止 `data_mountpoint=/`、`data_device` 与槽/整盘重名及 `max_evidence_age_ms > 5000`；`r3c_coordinator_confirm()` 改为**探针完成后**采集单调时间 `sample_end` 并校验单次采样耗时 `<= max_gap_ms` | `r3c_data_exclusion_test` PASS；全套 `test_g2r3c_n09_n10` 7/7 OK |

---

## 2. 本轮全量主机复测结果（最终代码修改后重跑）

所有原始日志保存于 [g2-r3c3-host-logs/](g2-r3c3-host-logs/)：

| 测试项 | 命令 | 结果 | 原始日志 |
|---|---|---|---|
| `boot_ab` C 套件 + R3C 数据/签名 + ARM 检查 | `make -C bsp/apps/boot_ab clean test arm-check arm-r3c-check` | `ab_state` `100/100`、`fw_env_backend` `31/31`、`ab_health FAIL=0`、`system_health` `195/195`、`health_devices` `21/21`、`health_network` `5/5`、`r3c_binding` `16/16`、`r3c_data_exclusion` `PASS`；`test_g2r3c_data` `7/7`、`test_g2r3c_signed_bundle` `5/5`；ARM ELF32 OK | [boot-ab.log](g2-r3c3-host-logs/boot-ab.log) |
| **N09 / N10 端到端闭环与安全复测** | `python3 -m unittest -v tests.test_g2r3c_n09_n10` | **7/7 OK**（`8.764 s`） | [n09-n10.log](g2-r3c3-host-logs/n09-n10.log) |
| FUSE ext4 跨槽持久化 | `python3 -m unittest -v tests.test_g2r3c_fuse_image` | **1/1 OK** | [fuse-image.log](g2-r3c3-host-logs/fuse-image.log) |
| G1 / G2 / R3 核心回归 | `python3 -m unittest -v tests.test_g1_backup_guard tests.test_g2_uboot_ab tests.test_g2r3_health` | **25/25 OK** | [g1-g2-r3.log](g2-r3c3-host-logs/g1-g2-r3.log) |
| Gateway 探针回归 | `python3 -m unittest -v tests.test_g2r3b_gateway_probe` | **2/2 OK** | [gateway-probe.log](g2-r3c3-host-logs/gateway-probe.log) |
| G3 内核与 RootFS 指标回归 | `python3 -m unittest -v tests.test_g3_kernel_rootfs` | **2/2 OK** | [g3.log](g2-r3c3-host-logs/g3.log) |
| 网关核心 C 单测 & ARM 编译 | `make test`、`make gateway arm` | `10/10 PASS`、ARM `smart_gateway` OK | [core.log](g2-r3c3-host-logs/core.log)、[gateway-arm.log](g2-r3c3-host-logs/gateway-arm.log) |
| Service Supervisor & OTA Agent | `make -C bsp/apps/service_supervisor test`、`make -C bsp/apps/ota_agent test-supervisor test-supervisor-s2` | Supervisor `58/58`、`35/35`；OTA `145/145`、`44/44` | [supervisor.log](g2-r3c3-host-logs/supervisor.log)、[ota.log](g2-r3c3-host-logs/ota.log) |
