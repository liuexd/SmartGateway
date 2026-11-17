# G2-R3C 主机端闭环验收报告（DATA、可信事务、Gateway 策略解耦与 N09/N10）

- **日期：** 2026-11-17
- **分支：** `feature/imx6ull-system-ab-ota`（基于 `b5057f2` 推进收口）
- **主机侧结论：** **`HOST-ACCEPTED`**（共享 DATA 校验与槽位排除、完整可信事务生命周期、Gateway 机制/策略解耦、`sgw_bootctl confirm` 生产协调器及 N09/N10 端到端回归全部在主机隔离环境下闭环通过，ARMv7 交叉编译与 ELF 依赖检查通过）
- **板端真机状态：** **`BOARD-PENDING / BLOCKED`**（本轮未触碰开发板 `/dev/mmcblk1`、真实 U-Boot 环境、正式 `/etc/smartgateway/` 或 Watchdog；真实硬件块挂载、真机 `bootcmd` 与掉电串口取证仍受 G1 恢复门禁约束）

---

## 1. 四项核心缺口收口情况

| 事项 | 本轮收口实现 | 主机证据 |
|---|---|---|
| **共享 DATA** | 新增 `r3c_data_check_slot_exclusion_sysfs` 与 `r3c_data_inspect_full`：校验 block `major:minor`、sysfs 分区起止扇区不与 `p1`/`p2`/整盘或 U-Boot 环境保留扇区（`1792..1824`）重叠；新增 `r3c_data_verify_persistence` 执行写-`fsync`-读回-删探针；协调器全程持有 `.ota.lock` 共享锁 + `.sgw-transaction.lock` 排他锁 | `r3c_data_exclusion_test` PASS；`test_g2r3c_data` 7/7 OK；`test_g2r3c_fuse_image` 1/1 OK；`test_g2r3c_n09_n10` 使用真实 `fuse2fs` (`fuse.ext4`) 挂载、写/`fsync` 读回与 `.ota.lock` 冲突拒绝 |
| **可信事务与确认协调器** | 新增 `r3c_txn.{h,c}` 与 `r3c_coordinator.{h,c}`：将 `r3c_bundle_record`（RSA-2048/SHA-256）串接为完整状态机：`VERIFIED` -> 写槽+`fsync`+读回摘要校验 (`WRITTEN`) -> `ab_stage_trial`+`fw_env` 读回验证 (`PENDING`) -> `boot_id`/`bootcount` 绑定与多轮稳定观察 -> `CONFIRMING` -> `ab_confirm_trial`+`fw_env` 读回校验 -> `CONFIRMED`（`consumed=1` 抗重放）。生产 `sgw_bootctl confirm` 已接入该协调器并默认 Fail-Closed | `test_g2r3c_signed_bundle` 5/5 OK；`test_g2r3c_n09_n10` 4/4 OK（含写槽篡改检测、消费后重放拒绝、`r3c_fsync_fail` 及 `.txn-` 精确 `fsync` 失败拦截） |
| **Gateway 机制与策略解耦** | 保留 `SGW-PROBE/1` 与 `SGW-FLOWX/1`（无额外上行报文），在 `gateway/server_link.c` 与 `gateway/health_probe.c` 新增工作线程原子心跳及 `SGW-LOCAL/1` 本地健康检查；新增根/属主受保护策略 `r3c_policy.{h,c}`（`/etc/smartgateway/r3c-policy.conf`），默认 `strict_forward` + `allow_external_degraded=0`，可由受审配置切换 `controlled_local` + `allow_external_degraded=1` | `test_g2r3b_gateway_probe` 2/2 OK；`test_g2r3c_n09_n10` 同时验证 `strict_forward`（真实转发 DATA 后放行，无流量拒绝）与 `controlled_local` 策略开关（`0` 拒、`1` 放） |
| **N09 / N10 全链闭环** | 编译隔离测试驱动 `r3c_confirm_harness`（调用同一套生产 `r3c_coordinator_confirm`、`r3c_data`、`r3c_txn`、`r3c_policy`、`ab_state`、`fw_env_backend` 代码），联调真实 `service_supervisor` + `smart_gateway` + `fuse2fs` DATA + `fw_printenv`/`fw_setenv` | `test_g2r3c_n09_n10` 4/4 OK（覆盖 N09 正常确认及 N10 重放、篡改、策略越权、锁冲突、传感器/网口异常、Gateway 停止、`fsync` 落盘失败） |

---

## 2. 本轮回归测试与原始日志

所有测试均在最终代码编辑完成后复跑通过，原始输出保存于 [g2-r3c3-host-logs/](g2-r3c3-host-logs/)：

| 测试项 | 命令 | 结果 | 原始日志 |
|---|---|---|---|
| `boot_ab` C 套件 + R3C 数据/签名 + ARM 检查 | `make -C bsp/apps/boot_ab clean test arm-check arm-r3c-check` | `ab_state` `100/100`、`fw_env_backend` `31/31`、`ab_health` `FAIL=0`、`system_health` `195/195`、`health_devices` `21/21`、`health_network` `5/5`、`r3c_binding` `16/16`、`r3c_data_exclusion` `PASS`；`test_g2r3c_data` `7/7`、`test_g2r3c_signed_bundle` `5/5`；ARM ELF32 构建与库依赖 check OK | [boot-ab.log](g2-r3c3-host-logs/boot-ab.log) |
| **N09 / N10 端到端闭环** | `python3 -m unittest -v tests.test_g2r3c_n09_n10` | **4/4 OK**（`6.989 s`） | [n09-n10.log](g2-r3c3-host-logs/n09-n10.log) |
| FUSE ext4 跨槽持久化 | `python3 -m unittest -v tests.test_g2r3c_fuse_image` | **1/1 OK** | [fuse-image.log](g2-r3c3-host-logs/fuse-image.log) |
| G1 / G2 / R3 核心回归 | `python3 -m unittest -v tests.test_g1_backup_guard tests.test_g2_uboot_ab tests.test_g2r3_health` | **25/25 OK** | [g1-g2-r3.log](g2-r3c3-host-logs/g1-g2-r3.log) |
| Gateway 探针回归 | `python3 -m unittest -v tests.test_g2r3b_gateway_probe` | **2/2 OK** | [gateway-probe.log](g2-r3c3-host-logs/gateway-probe.log) |
| G3 内核与 RootFS 指标回归 | `python3 -m unittest -v tests.test_g3_kernel_rootfs` | **2/2 OK** | [g3.log](g2-r3c3-host-logs/g3.log) |
| 网关核心 C 单测 & ARM 编译 | `make test`、`make gateway arm` | `10/10 PASS`、ARM `smart_gateway` OK | [core.log](g2-r3c3-host-logs/core.log)、[gateway-arm.log](g2-r3c3-host-logs/gateway-arm.log) |
| Service Supervisor & OTA Agent | `make -C bsp/apps/service_supervisor test`、`make -C bsp/apps/ota_agent test-supervisor test-supervisor-s2` | Supervisor `58/58`、`35/35`；OTA `145/145`、`44/44` | [supervisor.log](g2-r3c3-host-logs/supervisor.log)、[ota.log](g2-r3c3-host-logs/ota.log) |

---

## 3. 仍然保留的物理与后续阶段边界（未越界宣称）

1. **真实块设备与硬件测试需在板端复核（`BOARD-PENDING`）：** 主机非特权环境下无法挂载真实 `/dev/mmcblk1p3` 至真实块设备根路径，因此 `r3c_confirm_harness` 在端到端测试中使用了 `fuse2fs` 挂载实际 `ext4` 镜像、独立 `sysfs`/硬件节点夹具，并用独立的 `r3c_data_exclusion_test` 验证 `sysfs` 块几何排重。真机上线前仍需在完成 G1 离线备份与外部 SD 恢复演练后执行板端实测。
2. **G4 对接接口已就绪：** G4 阶段只需要把正式 SWUpdate `.swu` / `sw-description` 签名拆解/写槽流程接入 `r3c_bundle_record` -> `r3c_txn_write_and_verify_slot` -> `r3c_txn_stage_pending`，并在 RootFS 中安装受根权限保护的 `/etc/smartgateway/r3c-policy.conf` 与公钥即可。
