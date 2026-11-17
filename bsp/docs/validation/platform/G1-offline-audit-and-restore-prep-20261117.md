# G1：离线分段审计、引导区定位与恢复工具验证（2026-11-17）

**结论：主机侧离线审计（U04/U05/boot0）、工程化恢复镜像 `candidate-v2` 与分段写回校验工具均已通过主机验证；G1 板端终验仍为 `BLOCKED`，等待最后两项板端门槛完成。**

## 1. 离线实证结论（基于 `backup-20261117` 与板端只读 `EXT_CSD`）

1. **真实 U-Boot 引导硬件区定位（极关键）：**
   - 板端 `mmc extcsd read /dev/mmcblk1` 实测 `PARTITION_CONFIG: 0x48`（`Boot Partition 1 enabled`）。
   - 离线审计（[g1-20261117-offline-audit.log](g1-20261117-offline-audit.log)）证实：`emmc-boot0.img` `@0x400` 存放当前实机运行的 `U-Boot 2017.03-g8ba4c5bb19-dirty (Jun 10 2026)`，且其 `523,264` 字节本体与 `p2` 内 `/root/u-boot-dtb.imx` 的 SHA-256（`ad9fb499ba457d1e121bd840483afce5fd270e8bc8fd0eee3af331c84bcba5e3`）完全一致。
   - `emmc-reserved-pre-p1.bin` `@0x400` 仅存 2024-07-21 旧版 U-Boot；`@0xC0000` 为旧版 46 变量环境；**`@0xE0000` 存放当前生效的 44 变量环境**（CRC32=`0x7d67365e` 通过，含 `mmcdev=1`、`mmcpart=2`、`mmcroot='/dev/mmcblk1p2 rootwait rw'`、`ethaddr`、`eth1addr`、`panel=TFT7016`、`update=yes`）；`@0xE2000` 全零可用。
   - 因此，完整恢复**必须同时恢复 `user area` 与 `mmcblk1boot0`（临时解除 `force_ro` 后写回并恢复只读）**。
2. **U04（`p1` / `p3` / `tail` 用途实证）：**
   - `emmc-p1.img`（1,000 MiB）：前 `50 MiB`（`52,429,824` 字节）全零，`+50 MiB` 起为重分区遗留的 2021-12-17 旧版 ext4（`superblock` 在 `+50 MiB + 1024`，`179,200` 块），从 `p1` 起始偏移无法直接挂载，当前未被使用。
   - `emmc-p3.img`（500 MiB）：全盘 `500/500` MiB **100% 全零**（`0x00`），无任何用户数据或文件系统。
   - `emmc-tail.img`（726 MiB）：仅含 `4/726` 个 1 MiB 块非零（旧 ext4 备份超级块残留），`+1024` 无有效超级块。
3. **U05 与唯一数据盘点：**
   - `emmc-p2.img`（1,500 MiB）：健康 `ext4`（`384,000` 块，`used_bytes=749,178,880`），含 `/boot/{zImage,100ask_imx6ull-14x14.dtb,...}`、`/etc/swupdate/{swupdate.cfg,swupdate-public.pem,postupdate.sh}`、`/opt/smartgateway/{bin,releases,current,state,packages,service.conf,...}`、`/etc/ssh/ssh_host_*`；未发现任何 `.swu` 或 `sw-description`，确认走“自建受控 A/B 升级链”路线。

## 2. 工程化恢复镜像与写回校验工具（主机验证通过）

- **工程化内存恢复镜像 `candidate-v2`（[g1_prepare_recovery_initramfs.py](../../../tools/g1_prepare_recovery_initramfs.py)）：**
  - 修复 `candidate-mini` 中 `/init` 因静态 `/dev/console` 存在而跳过 `devtmpfs` 挂载的缺陷；
  - 修复 `/usr/bin/g1_start_ssh.sh` 未使用绝对路径 `/usr/sbin/sshd` 导致的 `re-exec` 失败；
  - 内置 `/usr/bin/g1_set_emmc_ro.py` 与 `g1_restore_segments.py`，保持零自动挂载与零自动写入 eMMC；
  - 生成 `/home/ubuntu/.sgw-g1-recovery-prep/candidate-v2/boot/rescue-initramfs-v2.cpio.gz`（`14,888,734` 字节）并通过 `sha256sum -c SHA256SUMS`。
- **防误写分段写回与回读校验工具（[g1_restore_segments.py](../../../tools/g1_restore_segments.py)）：**
  - 写前强制调用 `g1_verify_segments.verify()`；拒绝符号链接、已挂载设备以及 `mmcblk1p2` / 活跃 `/` 设备；`boot0`/`boot1` 通过 `ForceRoGuard` 临时关闭 `force_ro` 并在退出时恢复；每次写后 `fsync` 并只读重开回读比对 SHA-256。
  - 在隔离普通文件模式下对真实 `backup-20261117` 执行 7 段全盘写回测试（[g1-20261117-restore-sim.log](g1-20261117-restore-sim.log)），恢复后 user area（`3f7e250c...`）、`boot0`（`40ea8aa4...`）、`boot1`（`af805de7...`）SHA-256 逐字节一致。
- **单元测试：** `test_g1_backup_guard`、`test_g1_verify_segments`、`test_g1_network_transport`、`test_g1_ssh_loopback`、`test_g1_audit_offline_backup`、`test_g1_restore_segments`、`test_g1_recovery_initramfs` 共 **32/32 PASS**。

## 3. G1 仍待完成的最后两项板端门槛（`BLOCKED`）

1. **不依赖 eMMC 的独立恢复入口实测（U02）：** 验证通过 USB OTG Serial Download Mode（SDP / `uuu` 或 `imx_usb`）或独立 SD 卡在不依赖 `boot0`/`pre-p1` 的条件下直接将 U-Boot + 内核 + DTB + `candidate-v2` 加载至内存进入恢复 shell。
2. **备份可恢复性与恢复后启动验证（U03 / 门槛 #5）：** 在不破坏现用 `p2` 的前提下，完成等价全盘恢复启动验证（如 QEMU）和/或经单独授权的废弃 `p1` 非破坏性写回与实机启动验收（串口、Kernel、根挂载、ADB/网络、GPIO/I²C、Gateway/Supervisor）。
