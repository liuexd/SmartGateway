# G4 出厂 p2 保护、共享 DATA (p3) 门禁补齐与 ARM RootFS 重建验收报告（2026-11-19）

> **状态：** `HOST-ACCEPTED`（双门禁主机验证通过，且已使用 `CONFIG_SIGNED_IMAGES=y` + `CONFIG_SIGALG_RAWRSA=y` 与更新后的 ARM `sgw_bootctl` 重建最终 ARM RootFS 候选镜像；真机未操作，真实 `/dev/mmcblk1p2` 保持出厂黄金恢复基线不写）。

## 1. 补齐的两项生产写入门禁

1. **显式出厂 `p2` 写保护门禁：**
   - [r3c_policy.h](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_policy.h) 与 [r3c_policy.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_policy.c) 新增策略字段 `protect_slot2`（默认 fail-closed 设为 `1`，支持 `r3c-policy.conf` 显式配置并拒绝重复/非法值）。
   - [r3c_apply.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_apply.c) 在打开 `txdir`、槽设备或 U-Boot env 之前优先检查 `if (policy->protect_slot2 && opts->target_slot == 2)` 并直接拒绝（`"target slot 2 is write-protected factory baseline"`）。
   - 生产模式 [sgw_bootctl.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/sgw_bootctl.c)（未定义 `SGW_BOOTCTL_ISOLATED_SIM`）及生产 SWUpdate root 钩子 [sgw-swu-apply.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/sgw-swu-apply.sh) 均强制拒绝 `--target 2` 并锁死 `r3c_pol.protect_slot2 = 1`；[g4_swu_tool.py](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/ota/g4_swu_tool.py) 默认写入 `protect_slot2=1`，仅当测试显式传入 `--allow-virtual-slot2` 时才允许模拟 `p1 -> p2` 虚拟盘验证。

2. **写槽前共享 `p3` DATA 身份、schema 与隔离证明门禁：**
   - [r3c_apply.h](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_apply.h) 与 [r3c_apply.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_apply.c) 在 `verify_data_and_open_txdir()` 中于 `r3c_bundle_record()` 及写槽之前执行四重核验：
     1. 校验 `policy->tx_dir` 路径严格位于 `policy->data_mountpoint/` 之下；
     2. 生产模式（`!allow_fuse_data_bypass_blk`）直接复用 [r3c_data.c](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/apps/boot_ab/r3c_data.c) 的 `r3c_data_inspect_full()`（`ext4` mountinfo、`/dev/mmcblk1p3` sysfs 与 `p1`/`p2`/U-Boot 环境扇区不重叠、`schema=1\n`、剩余空间与持久化探针 `fsync`）；
     3. 主机隔离 FUSE 测试模式（仅在 `sgw_bootctl_isolated` + `--fuse-data`）校验独立挂载点 `dst.st_dev != root.st_dev`、`r3c_data_mountinfo_matches_fstype(..., "fuse.ext4")`、`r3c_data_inspect_contents()`（`schema=1\n`）及 `r3c_data_verify_persistence()`；
     4. 通过内核 VFS `fstat()` 校验 `txst.st_dev == dst.st_dev && txst.st_dev != root.st_dev`，确保事务目录真实落在已验证的 DATA 挂载上。

## 2. ARM SWUpdate + 最终 RootFS 重建审计

已在主机离线执行 `make -C build/buildroot-g3 swupdate-dirclean && ./bsp/rootfs/build_rootfs_g3.sh`，并在 [build_rootfs_g3.sh](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/rootfs/build_rootfs_g3.sh) 及 [test_g3_kernel_rootfs.py](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/tests/test_g3_kernel_rootfs.py) 中固化自动化成品校验：
- **SWUpdate Kconfig：** `BR2_PACKAGE_SWUPDATE_CONFIG=".../bsp/ota/swupdate.config"`；`build/buildroot-g3/build/swupdate-2019.11/.config` 已启用 `CONFIG_SIGNED_IMAGES=y`、`CONFIG_SIGALG_RAWRSA=y`、`CONFIG_HASH_VERIFY=y`、`CONFIG_HW_COMPATIBILITY=y`、`CONFIG_BOOTLOADER_NONE=y`，且 `# CONFIG_UBOOT is not set`、`# CONFIG_BOOTLOADERHANDLER is not set`。
- **提取成品特征与 SHA-256：**
  - `build/buildroot-g3/target/usr/bin/swupdate`（ARM ELF32，SHA-256 `d4280cf11e5b785a5d038878bf43a8e5f9802f1a72de38ef10abb397b5f5ca78`）：包含 `"swupdate built for signed image, provide a public key file"`，不含 `"public key / cert %s ignored"`。
  - `build/arm-ab/sgw_bootctl`（ARM ELF32，`GLIBC_2.28`，355,380 字节，SHA-256 `d3877400d8d66b73870c75685fb157d082a21d7b4a5a560d9eba472d0ae261a3`）：与 `rootfs.tar` 内 `usr/bin/sgw_bootctl` 摘要完全一致，包含 `"target slot 2 is write-protected factory baseline"`。
  - `/etc/hwrevision` 为 `imx6ull-100ask revA`，`/etc/swupdate/swupdate.cfg` 仅保留本地签名配置，`/usr/bin/sgw-swu-install` 为 `0:0`，厂商遗留 `etc/swupdate/postupdate.sh` 与 `swupdate-public.pem` 已移除。
  - `rootfs.ext4`：`134217728` 字节，SHA-256 `e120306977e5038df74ac904e86fd9f83ee17339b1dee3302a53f1a5a37e6a47`；`rootfs.tar`：`34723840` 字节（内含文件 `32581918` 字节，1303 项），`g3_trimmed_vs_core_packages.diff` 零变化。

## 3. 全量实测结果（全部在最终编辑及镜像重建后执行通过）

| 检查项 | 实测命令与结论 | 归档日志 |
| --- | --- | --- |
| `boot_ab` x86-64 / 隔离构建 / ARM 与 C/R3C 单测 | `make -C bsp/apps/boot_ab clean all sgw_bootctl_isolated arm-check test` -> `ab_state_test` 100/100、`fw_env_backend_test` 31/31、`ab_health_test` FAIL=0、`system_health_test` 195/195、`health_devices_test` 21/21、`health_network_test` 5/5、`r3c_binding_test` 16/16、`r3c_data_exclusion_test` PASS、`test_g2r3c_data` 7/7 OK、`test_g2r3c_signed_bundle` 5/5 OK | [01-boot-ab-build-and-c-tests.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-gates-arm-logs/01-boot-ab-build-and-c-tests.log) |
| ARM RootFS 重建与成品审计 | `make -C build/buildroot-g3 swupdate-dirclean && ./bsp/rootfs/build_rootfs_g3.sh` + `sha256sum`/`file`/`strings` -> 27s 完成并通过全部签名配置与哈希检验 | [02-arm-rootfs-rebuild-and-audit.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-gates-arm-logs/02-arm-rootfs-rebuild-and-audit.log) |
| G4 真实 SWUpdate 隔离专项 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_g4_swu_isolated.py` -> **9/9 OK**（`9.531s`，含 `p2` 写保护与 `p3` DATA 全故障矩阵） | [03-test-g4-swu-isolated.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-gates-arm-logs/03-test-g4-swu-isolated.log) |
| G3 RootFS 制品校验与 Review4 回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest -v tests/test_g3_kernel_rootfs.py tests/test_review4_g3_bundle.py tests/test_review4_topology.py` -> **6/6 OK**（`1.086s`） | [04-test-g3-and-review4.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-gates-arm-logs/04-test-g3-and-review4.log) |
| 全量 `test_g*.py` 主机回归 | `SGW_TEST_FUSE2FS=/usr/bin/fuse2fs python3 -m unittest discover -s tests -p 'test_g*.py' -v` -> **96/96 OK**（`86.618s`） | [05-test-g-suite.log](file:///home/ubuntu/Desktop/project/SmartGateway-BSP/bsp/docs/validation/platform/g4-20261119-gates-arm-logs/05-test-g-suite.log) |

## 4. 边界声明

- 本次完成的是 **主机端 `p2` 写保护、`p3` 共享 DATA 门禁以及 ARM RootFS（含签名版 SWUpdate）重建验证**。
- 开发板未作写入操作，真机 `/dev/mmcblk1p2` 继续保留为出厂黄金恢复基线；板端候选槽 G4 验收仍待另行授权安排，且需使用上述重建后的新候选 RootFS（部署好受保护的 `/etc/smartgateway/ota-public.pem` 与 `/etc/smartgateway/r3c-policy.conf`）。
