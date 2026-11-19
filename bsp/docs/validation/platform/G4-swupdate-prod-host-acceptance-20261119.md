# G4 SWUpdate 生产入口集成与隔离主机验收报告（2026-11-19）

> **状态：** `HOST-ACCEPTED`（真实 `swupdate 2019.11` 生产入口 + `sgw_bootctl apply-bundle` 隔离虚拟盘全链路验收通过；开发板未操作，真实 `/dev/mmcblk1p2` 保持出厂黄金恢复基线不写）。

## 1. 生产链路架构与改动摘要

本次已将 G4 主机隔离链路中的测试替代入口 `r3c_confirm_harness` 彻底替换为 **真实 SWUpdate 2019.11 生产入口 + 生产 `sgw_bootctl apply-bundle` / `r3c_coordinator_apply_bundle`**：

```text
[g4_swu_tool.py pack (-H crc)]
   └─► SWUpdate .swu (sw-description [1st], sw-description.sig [2nd], sgw-swu-apply.sh, manifest, manifest.sig, rootfs.manifest, zImage, 100ask_imx6ull-14x14.dtb, rootfs.img)
         │
         ▼
[swupdate 2019.11 (CONFIG_SIGNED_IMAGES=y, CONFIG_SIGALG_RAWRSA=y, CONFIG_HASH_VERIFY=y, CONFIG_BOOTLOADER_NONE=y)]
   1) 提取并 RSA-2048 验签 sw-description.sig（拒绝无签名/错公钥/篡改签名/错硬件版本/非法 bootenv）
   2) 逐文件校验 SHA-256（rootfs.img, zImage, dtb, rootfs.manifest, manifest, manifest.sig, sgw-swu-apply.sh）
   3) rawfile 提取到私密 0700 ./bundle 并调用 shellscript sgw-swu-apply.sh postinst
         │
         ▼
[sgw_bootctl apply-bundle -> r3c_coordinator_apply_bundle()]
   1) 验证 0700 bundle 目录并先抢占 .ota.lock 排他锁，校验应用 OTA 处于终态（CONFIRMED/ROLLED_BACK）
   2) 独立核验运行槽与冗余 fw_env 状态（拒绝在 trial/upgrade_available=1 或同槽/last_good 写入）
   3) 校验信任 tx_dir/pubkey、zImage & dtb 与 rootfs.manifest 一致性，及目标槽容量预检
   4) 调用 r3c_bundle_record()（SGW-SYSTEM-2 验签 + debugfs 15 项强制系统文件逐个摘要比对）-> [VERIFIED]
   5) 调用 r3c_txn_write_and_verify_slot()（写槽 + fsync + 读回校验）-> [WRITTEN]；若写入/fsync 失败则回收未提交 VERIFIED 事务并拒绝阶段提交
   6) 调用 r3c_txn_stage_pending()（写冗余 U-Boot env 0xE0000/0xE2000 试启动 + 读回验证）-> [PENDING]
```

关键交付文件：
- **SWUpdate 生产配置与主机编译**：[swupdate.config](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/swupdate.config)、[swupdate.cfg](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/swupdate.cfg)、[build_swupdate_host.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/build_swupdate_host.sh)
- **SWUpdate 钩子、安装器与打包工具**：[sgw-swu-apply.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/sgw-swu-apply.sh)、[sgw-swu-install.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/sgw-swu-install.sh)、[g4_swu_tool.py](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/g4_swu_tool.py)
- **生产写入协调器与 `sgw_bootctl`**：[r3c_apply.h](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_apply.h)、[r3c_apply.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_apply.c)、[sgw_bootctl.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/sgw_bootctl.c)、[Makefile](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/Makefile)
- **G3/G4 镜像构建配置接线**：[build_rootfs_g3.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/rootfs/build_rootfs_g3.sh)、[post_build_g3.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/rootfs/post_build_g3.sh)
- **隔离验收测试集**：[test_g4_swu_isolated.py](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/tests/test_g4_swu_isolated.py)

## 2. 实测验证结果（全部在最终编辑后重跑通过）

| 检查项 | 命令与输出摘要 | 日志归档 |
| --- | --- | --- |
| 主机真实 `swupdate 2019.11` 离线构建与签名检查 | `./bsp/ota/build_swupdate_host.sh` -> `Swupdate v2019.11.0`；确认内置强制签名检查且无忽略公钥回退 | [01-build-swupdate-host.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-host-logs/01-build-swupdate-host.log) |
| `boot_ab` x86-64 / `sgw_bootctl_isolated` / ARM 构建与 C/R3C 单测 | `make -C bsp/apps/boot_ab clean all sgw_bootctl_isolated arm-check test` -> ARM `GLIBC_2.28` 无新增依赖；`ab_state_test` 100/100、`fw_env_backend_test` 31/31、`ab_health_test` FAIL=0、`system_health_test` 195/195、`health_devices_test` 21/21、`health_network_test` 5/5、`r3c_binding_test` 16/16、`r3c_data_exclusion_test` PASS、`test_g2r3c_data` 7/7 OK、`test_g2r3c_signed_bundle` 5/5 OK | [02-boot-ab-build-and-c-tests.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-host-logs/02-boot-ab-build-and-c-tests.log) |
| G4 真实 SWUpdate 隔离虚拟盘专项验收 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_g4_swu_isolated.py` -> **7/7 OK**（`7.302s`） | [03-test-g4-swu-isolated.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-host-logs/03-test-g4-swu-isolated.log) |
| 全量 `test_g*.py` 主机回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest discover -s tests -p 'test_g*.py' -v` -> **94/94 OK**（`82.082s`） | [04-test-g-suite.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-host-logs/04-test-g-suite.log) |
| G3/R3C 实包与拓扑审查回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_review4_g3_bundle.py tests/test_review4_topology.py` -> **4/4 OK**（`0.812s`） | [05-test-review4.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-host-logs/05-test-review4.log) |

### G4 隔离验收覆盖明细（`tests/test_g4_swu_isolated.py` 7/7）
1. `test_p1_to_p2_real_swupdate_apply_and_pending_stage`：验证 `-f swupdate.cfg` 与 `-k` 公钥配合，真实 `swupdate` 验签并调用 `sgw-swu-apply.sh` -> `sgw_bootctl_isolated` 写槽校验，写入虚拟 `slot2` SHA-256 精确一致，`slot1` 不变，U-Boot 环境切为 `sgw_pending_slot=2`、`sgw_ab_state=trial`、`upgrade_available=1`，`.sgw-tx/txn-<id>` 为 `PENDING` 且 companion `txn-<id>.manifest` 为 `0600`。
2. `test_p2_to_p1_real_swupdate_apply_and_pending_stage`：反向 `p2 -> p1` 隔离写入验证通过，`slot2` 未被修改。
3. `test_real_g3_artifacts_swu_p1_to_p2_via_real_swupdate`：打包真实 128 MiB G3 `rootfs.ext4`、`zImage`、`100ask_imx6ull-14x14.dtb` 及 15 项签名清单，经真实 `swupdate` 写入 160 MiB 虚拟槽并通过字节级比对。
4. `test_negative_swupdate_rejections_zero_mutation`：无签名 `.swu`、非信任私钥签名、篡改 `sw-description.sig`、篡改 `rootfs.img`/`zImage`/`dtb`/`sgw-swu-apply.sh`（SWUpdate `HASH mismatch`）、错误板型 `other-board`、错误硬件修订 `revB`，以及试图注入 `bootenv` 直改 U-Boot 环境的签名 `.swu`（`bootloader support absent`）均被 SWUpdate 拒绝，源/目标槽、U-Boot 环境、`txdir` 零变更。
5. `test_negative_bootctl_rejections_zero_mutation`：写当前运行槽 `slot1`、小容量目标槽 `2 MiB`、并发 `.ota.lock` 锁冲突、活跃 `trial` 态禁止写入、外层重签但篡改内层 `manifest.sig`、以及顶层 `zImage` 与 `rootfs.manifest` 摘要分裂，均被 `sgw_bootctl apply-bundle` 拒绝且零变更、零残留 `txn-*`。
6. `test_fsync_failure_during_slot_write_never_stages_pending`：`LD_PRELOAD` 注入 `slot2.ext4` `fsync()` 返回 `EIO` 失败，确认永不提交 `PENDING` 与 U-Boot `trial` 环境并自动清理未完成事务记录；撤除故障注入后紧接重试一次通过，验证不会阻塞后续升级。
7. `test_production_sgw_bootctl_apply_bundle_rejects_unprivileged_and_overrides`：验证生产二进制 `bsp/apps/boot_ab/sgw_bootctl apply-bundle` 严格拒绝非 root 调用及 `-c`/`--policy`/`--slot-img` 覆盖参数（包括 `LD_PRELOAD` 伪造 `geteuid()=0` 场景）。

## 3. 边界与后续前置条件

1. **当前验收范围为隔离主机环境：** 全链路已真实覆盖 `swupdate 2019.11` -> `sgw-swu-apply.sh` -> `sgw_bootctl apply-bundle` (`r3c_coordinator_apply_bundle`)，但本次未在物理开发板上执行 `build_rootfs_g3.sh` 重建与真机刷写。
2. **真机 `/dev/mmcblk1p2` 保持保护：** 物理板未受任何操作，出厂恢复基线完好。
3. **板端后续前提：** 若后续计划做板端 G4 验收，需先执行 `bsp/rootfs/build_rootfs_g3.sh` 重新打包包含启用了 `CONFIG_SIGNED_IMAGES` 的目标端 `swupdate`、`/etc/hwrevision`（`imx6ull-100ask revA`）、`/usr/bin/sgw-swu-install` 及更新后 ARM `sgw_bootctl` 的 G3/G4 根文件系统。
