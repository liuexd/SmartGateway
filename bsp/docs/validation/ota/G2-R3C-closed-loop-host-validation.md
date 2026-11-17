# G2-R3C 主机端闭环与二次安全整改复验报告

- **日期：** 2026-11-17
- **分支：** `feature/imx6ull-system-ab-ota`
- **主机侧状态：** **`READY-FOR-INDEPENDENT-REVIEW`（已完成独立复审指出的 5 项安全与板端可用性整改，并全量复跑通过；未推送远端、未签发板端放行）**
- **板端真机状态：** **`BOARD-PENDING / BLOCKED`**（本轮未触碰开发板 `/dev/mmcblk1`、真实 U-Boot 环境或 Watchdog；G1 外部恢复与 G2 板端验收仍待授权执行）

---

## 1. 针对独立复审 5 项发现的整改与实测覆盖

| # | 复审发现的问题 | 本轮代码整改 | 实测验证 |
|---|---|---|---|
| **1** | **写槽接口 `r3c_txn_write_and_verify_slot` 未自行校验目标设备** | 重构接口接收 `fw_cfg` / `hcfg` / `r3c_slot_write_opts_t` 并由函数内部打开设备：写入 **1 个字节之前**先独立校验 `ab_detect_running_slot_full()`、`fw_env` 非试启动态、`target_slot == other(running_slot)`、父目录安全 + `!S_ISLNK` + `S_ISBLK`（生产模式禁止普通文件）、非根挂点/非运行槽/`last_good`、不在 `mountinfo` 挂载列表中，以及 `BLKGETSIZE64` / 容器容量 `>= src_len` | `test_n10_write_slot_rejects_running_mounted_and_small_with_zero_bytes_written`：覆盖运行槽冲突、容量不足、`symlink` 目标与 `--require-blk` 拒绝，断言触发金丝雀文件零字节改动且事务保持 `VERIFIED` |
| **2** | **DATA 路径信任检查未落在 `r3c_data_inspect_full` 且错把块设备当目录检查** | 在 `r3c_data.c` 将目录树校验拆为 `trusted_parent_dirs()`（校验目录链）与 `trusted_block_node()`（校验叶子节点 `!S_ISLNK && S_ISBLK && !(mode & 0002)`），并在 **`r3c_data_inspect()` 与 `r3c_data_inspect_full()` 两处同时调用** | `r3c_data_exclusion_test`：直接针对 VM 真实 `/dev/sda2` 及指向 `/dev/sda2` 的软链调用 `r3c_data_inspect_full()`，验证软链被 `trusted_block_node` 拒绝、真实块设备走通路径校验 |
| **3** | **DATA 排除遇到未知设备跳过，且把整盘当作区间重叠排重对象** | 重构 `r3c_data_check_slot_exclusion_sysfs()` 分离 `whole_disk_device` 与分区 `excluded_devices[]`：任一排除设备或整盘 `stat`/`trusted_block_node`/`read_sysfs_geom` 失败均**立即 Fail-Closed 拒绝**；仅对同盘分区比对 `[start, size)` 重叠、仅对同盘检查保留扇区 `1792..1824` | `r3c_data_exclusion_test`：在 VM 真实 `/dev/sda` + `/dev/sda1` + `/dev/sda2` + `/sys/dev/block` 上验证同盘合法分区通过、缺失/未知排除设备 `/dev/nonexistent_slot_xyz` 拒绝、整盘 `/dev/sda` 作为 DATA 拒绝、自身槽 `/dev/sda2` 拒绝 |
| **4** | **`rw` 启动后原生 RootFS 原始块 SHA-256 会变化，导致健康系统误被拒绝** | 在 `r3c_bundle.c` / `r3c_txn.c` / `r3c_coordinator.c` 新增 `SGW-SYSTEM-2`（含 RSA-2048 签名的 `rootfs.manifest` 不可变系统文件清单，原子留存 `txn-<id>.manifest`）与 `r3c_txn_verify_installed_rootfs()`：`WRITTEN` 写未挂载槽位时校验整盘读回 SHA-256；`CONFIRM` 时按 `rw`/`ro` 区分——`rw` 挂载强制要求签名 `rootfs.manifest` 并逐文件复核运行中 `sysroot` 不可变文件摘要；无 manifest 的 `SGW-SYSTEM-1` 在 `rw` 根下显式拒绝 | `test_n09_rw_ext4_mutated_after_boot_confirms_via_signed_rootfs_manifest_and_tamper_rejected`（通过 `fuse2fs` `rw` 挂载写入日志导致 `slot1.ext4` 原始 SHA-256 改变后仍安全确认；篡改 `/sbin/init` 拒绝）+ `test_n10_legacy_sgw_system_1_without_manifest_rejected_on_rw_root` |
| **5** | **`controlled_local` 此前只看工作线程心跳，未证明真实队列->转发线程消费且不可冒充完整业务健康** | 在 `gateway/health_probe.c` 与 `gateway/server_link.c` 为 `SGW-LOCAL/1 <nonce>` 接入真实 `upstream_queue -> server_link_worker` 本地标记消费验证（在 `server_link` 弹出队列即终止消费，零字节写入 `tcp_fd` 或上游）；同时策略与协调器严格保留语义边界：**`controlled_local` 下 `external` 永远为 `DEGRADED`（绝不标为 `HEALTHY`）**，仅当根属主受保护策略显式设置 `allow_external_degraded=1` 时按降级策略确认 | `test_g2r3b_gateway_probe` **2/2 OK**；`test_n09_controlled_local_policy_decoupling_without_upstream_traffic` 验证无上行流量时队列->工作线程消费生效，`allow_degraded=0` 拒绝、`allow_degraded=1` 以 `state=DEGRADED` 确认 |

---

## 2. 最终代码修改后的全量复测结果

所有测试均在本轮整改全部落盘后重新执行，原始输出保存于 [g2-r3c3-host-logs/](g2-r3c3-host-logs/)：

| 测试套件 | 命令 | 最新结果 | 原始日志 |
|---|---|---|---|
| `boot_ab` C 套件 + R3C + ARM 检查 | `make -C bsp/apps/boot_ab clean test arm-check arm-r3c-check` | `ab_state` `100/100`、`fw_env_backend` `31/31`、`ab_health` `FAIL=0`、`system_health` `195/195`、`health_devices` `21/21`、`health_network` `5/5`、`r3c_binding` `16/16`、`r3c_data_exclusion` `PASS`（含 `/dev/sda*` + `/sys/dev/block` 实测）；`test_g2r3c_data` `7/7`、`test_g2r3c_signed_bundle` `5/5`；ARM ELF32 OK | [boot-ab.log](g2-r3c3-host-logs/boot-ab.log) |
| **N09 / N10 端到端闭环与安全复测** | `python3 -m unittest -v tests.test_g2r3c_n09_n10` | **10/10 OK**（`10.888 s`） | [n09-n10.log](g2-r3c3-host-logs/n09-n10.log) |
| FUSE ext4 跨槽持久化 | `python3 -m unittest -v tests.test_g2r3c_fuse_image` | **1/1 OK** | [fuse-image.log](g2-r3c3-host-logs/fuse-image.log) |
| G1 / G2 / R3 核心回归 | `python3 -m unittest -v tests.test_g1_backup_guard tests.test_g2_uboot_ab tests.test_g2r3_health` | **25/25 OK** | [g1-g2-r3.log](g2-r3c3-host-logs/g1-g2-r3.log) |
| Gateway 探针回归 | `python3 -m unittest -v tests.test_g2r3b_gateway_probe` | **2/2 OK** | [gateway-probe.log](g2-r3c3-host-logs/gateway-probe.log) |
| G3 内核与 RootFS 指标回归 | `python3 -m unittest -v tests.test_g3_kernel_rootfs` | **2/2 OK** | [g3.log](g2-r3c3-host-logs/g3.log) |
| 网关核心 C 单测 & ARM 编译 | `make test`、`make gateway arm` | `10/10 PASS`、ARM `smart_gateway` OK | [core.log](g2-r3c3-host-logs/core.log)、[gateway-arm.log](g2-r3c3-host-logs/gateway-arm.log) |
| Service Supervisor & OTA Agent | `make -C bsp/apps/service_supervisor test`、`make -C bsp/apps/ota_agent test-supervisor test-supervisor-s2` | Supervisor `58/58`、`35/35`；OTA `145/145`、`44/44` | [supervisor.log](g2-r3c3-host-logs/supervisor.log)、[ota.log](g2-r3c3-host-logs/ota.log) |

---

## 3. 证据边界说明

- `test_g2r3c_n09_n10.py` 在无特权主机上对系统槽与 DATA 挂载点仍使用隔离目录夹具与 `fuse2fs`（真实 `/dev/mmcblk1p1..p3` 硬件块设备挂载及开发板启动链需在完成 G1 外部恢复验证后于开发板实测）。
- `r3c_data_exclusion_test.c` 已补充对真实内核 `/dev/sda`、`/dev/sda1`、`/dev/sda2` 及 `/sys/dev/block` 的直接验证。
