# G2-R3C 主机端闭环与三轮安全整改复验报告

- **日期：** 2026-11-17
- **分支：** `feature/imx6ull-system-ab-ota`
- **主机侧状态：** **`READY-FOR-INDEPENDENT-REVIEW`（针对 `b7832c7` 独立复审提出的 4 项安全与语义问题完成整改与回归，等待独立复审确认；未推送远端、未签发 `HOST-ACCEPTED`）**
- **板端真机状态：** **`BOARD-PENDING / BLOCKED`**（本轮未触碰开发板 `/dev/mmcblk1`、真实 U-Boot 环境或 Watchdog；G1 外部恢复与 G2 板端验收仍待授权执行）

---

## 1. 针对 `b7832c7` 独立复审 4 项问题的整改与实测覆盖

| # | 独立复审发现的问题 | 本轮代码整改 | 实测验证 |
|---|---|---|---|
| **1** | **签名清单未证明覆盖必需系统文件且未证明清单与镜像一致** | 在 `r3c_bundle.c` / `r3c_txn.c` 冻结 15 项强制系统文件集合（`init`、`inittab`、`fstab`、`fw_env.config`、`fw_printenv`、`sgw_bootctl`、`sgw_watchdog_guard`、`service_supervisor`、`smart_gateway`、`ota_agent`、`S15sgw_ab_guard`、`S99smartgateway`、`zImage`、`dtb`、`libc.so.6`），拒绝重复路径；验签阶段通过 `/usr/sbin/debugfs` 直接对 `rootfs.img` 描述符检查 ext4 魔数 `0xEF53`、`Type: regular` 并逐条比对镜像内实际文件 SHA-256 | `test_n10_incomplete_duplicate_or_image_mismatched_manifest_rejected`：分别验证移除 `/lib/libc.so.6`、写入重复 `/sbin/init`、篡改镜像内摘要后均即使带有效 RSA 签名也被 `r3c_bundle_cli` 拒绝 |
| **2** | **写槽检查未覆盖 sysfs 拓扑重叠与 `lstat -> open` 身份置换** | 在 `r3c_txn.h` / `r3c_txn.c` 新增 `read_blk_geom()` 与 `target_conflicts_with_mountinfo_or_slots()`，拒绝整盘目标、U-Boot 环境保留扇区（`1792..1824`）重叠、同盘已挂载分区或 `running`/`last_good` 重叠；打开描述符后交叉核验 `fstat` 与预查 `lstat` 的 `st_mode`/`st_dev`/`st_ino`/`st_rdev` 并二次复查拓扑与 `BLKGETSIZE64` | `test_n10_write_slot_mounted_blk_topology_overlap_and_toctou_swap`：实测 VM 真实 `/dev/sda2`（已挂载根分区）、`/dev/sda`（整盘）、合成 sysfs U-Boot 扇区重叠分区及 `LD_PRELOAD` `fstat`/`fstat64` TOCTOU 置换，均被拒绝且金丝雀镜像零字节改动 |
| **3** | **`SGW-LOCAL/1` 仅作队列消费未过完整业务解码链，且不可等同外发健康** | 在 `gateway/health_probe.c` 把 32 位随机十六进制 nonce 拆分为 `N1`/`N2` 两个 16 字节 KV 字段，在 `server_link_worker` 内完成 `message_json_decode_data -> message_json_build_data -> message_json_decode_data` 全程编解码往返校验（仍严禁写入 `tcp_fd`）；`r3c_coordinator.h/.c` 与 `sgw_bootctl.c` 显式标记 `local_pipeline_only=1`，`controlled_local` 永远汇报 `state=DEGRADED` | `test_n09_controlled_local_policy_decoupling_without_upstream_traffic` 验证 `local_pipeline_only=1` 与 `state=DEGRADED`；新增 `test_n09_local_pipeline_stalled_gateway_worker_rejected` 用 `SIGSTOP` 冻结 worker 验证消费停滞时 `confirm` 拒绝且 `sgw_ab_state` 保持 `trial` |
| **4** | **分裂状态崩溃恢复将未重新采样的系统标为 `HEALTHY`** | 修改 `r3c_coordinator.c` 恢复分支（`env=CONFIRMED && txn=CONFIRMING`）：校验镜像身份并将 `txn` 收尾为 `CONFIRMED`（`consumed=1`）时，保持 `samples=0` 并将 `overall_state` 明确设为 `SGW_HEALTH_UNKNOWN`；`sgw_bootctl` 打印 `RECOVERED_CONFIRMED ... ledger_reconciled=1 current_health=NOT_SAMPLED` | `test_n10_split_state_env_confirmed_txn_confirming_recovery_and_idempotency`：断言 `RECOVERED_CONFIRMED slot=1 last_good=1` 且 `state=UNKNOWN samples=0`（绝不出现 `state=HEALTHY`），并验证复查拒绝及幂等防重放 |

<details>
<summary>历史：上一轮（针对初版 5 项独立复审问题的整改）已包含在当前代码基线中</summary>

1. 写槽接口 `r3c_txn_write_and_verify_slot` 已内置运行槽、环境、`S_ISBLK`、容量及读回核验。
2. `r3c_data_inspect` 与 `r3c_data_inspect_full` 已同时接入 `trusted_parent_dirs()` 与 `trusted_block_node()`。
3. `r3c_data_check_slot_exclusion_sysfs()` 对未知排除设备与整盘实施 Fail-Closed 拒绝。
4. `SGW-SYSTEM-2` 与 `rootfs.manifest` 解决 `rw` 启动块摘要漂移误拒问题。

</details>

---

## 2. 本轮最终代码修改后的复测结果

所有测试均在完成本轮 4 项代码与用例修改后重新执行，原始输出保存于 [g2-r3c3-host-logs/](g2-r3c3-host-logs/)：

| 测试套件 | 命令 | 最新结果 | 原始日志 |
|---|---|---|---|
| `boot_ab` C 套件 + R3C + ARM 检查 | `make -C bsp/apps/boot_ab clean test arm-check arm-r3c-check` | `ab_state` `100/100`、`fw_env_backend` `31/31`、`ab_health` `FAIL=0`、`system_health` `195/195`、`health_devices` `21/21`、`health_network` `5/5`、`r3c_binding` `16/16`、`r3c_data_exclusion` `PASS`；`test_g2r3c_data` `7/7`、`test_g2r3c_signed_bundle` `5/5`；ARM ELF32 OK | [boot-ab.log](g2-r3c3-host-logs/boot-ab.log) |
| **N09 / N10 端到端闭环与安全复测** | `python3 -m unittest -v tests.test_g2r3c_n09_n10` | **13/13 OK**（`16.248 s`） | [n09-n10.log](g2-r3c3-host-logs/n09-n10.log) |
| FUSE ext4 跨槽持久化 | `python3 -m unittest -v tests.test_g2r3c_fuse_image` | **1/1 OK**（`0.148 s`） | [fuse-image.log](g2-r3c3-host-logs/fuse-image.log) |
| G1 / G2 / R3 核心回归 | `python3 -m unittest -v tests.test_g1_backup_guard tests.test_g2_uboot_ab tests.test_g2r3_health` | **25/25 OK**（`1.127 s`） | [g1-g2-r3.log](g2-r3c3-host-logs/g1-g2-r3.log) |
| Gateway 探针回归 | `python3 -m unittest -v tests.test_g2r3b_gateway_probe` | **2/2 OK**（`7.006 s`） | [gateway-probe.log](g2-r3c3-host-logs/gateway-probe.log) |
| 网关核心 C 单测 & ARM 编译 | `make test`、`make gateway arm` | `10/10 PASS`、ARM `smart_gateway` OK | [core.log](g2-r3c3-host-logs/core.log)、[gateway-arm.log](g2-r3c3-host-logs/gateway-arm.log) |
| Service Supervisor & OTA Agent | `make -C bsp/apps/service_supervisor test`、`make -C bsp/apps/ota_agent test-supervisor test-supervisor-s2` | Supervisor `58/58`、`35/35` PASS；OTA `145/145`、`44/44` PASS | [supervisor.log](g2-r3c3-host-logs/supervisor.log)、[ota.log](g2-r3c3-host-logs/ota.log) |
| G3 内核与 RootFS 指标回归 | `python3 -m unittest -v tests.test_g3_kernel_rootfs` | **0/2 FAILED**（因本轮执行 `make clean` 清除了 `build/kernel-g3/g3_kernel_metrics.json` 与 `build/buildroot-g3/g3_rootfs_metrics.json`，本轮未改 G3 代码亦未重编内核/Buildroot） | [g3.log](g2-r3c3-host-logs/g3.log) |

---

## 3. 证据边界与未完成门禁

- 主机隔离回归已覆盖 15 项系统必需文件校验、`debugfs` 镜像内摘要校验、真实 `/dev/sda`/`/dev/sda2` 拒绝、合成 sysfs 扇区重叠拒绝与 `LD_PRELOAD` `lstat->open` 身份替换拒绝；真实 `/dev/mmcblk1p1..p5` 块设备、`WDOG1` 硬件看门狗与串口真机重启仍待 **G1 外部恢复验证与板端真机授权后执行**。
- `tests.test_g3_kernel_rootfs` 需在重新执行 `bsp/kernel/build_kernel_g3.sh` 与 `bsp/rootfs/build_rootfs_g3.sh` 生成 `build/kernel-g3` 与 `build/buildroot-g3` 指标产物后方可复测通过。
