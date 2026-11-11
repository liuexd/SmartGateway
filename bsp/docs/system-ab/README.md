# i.MX6ULL System Customization and A/B OTA

本目录是当前系统级 BSP 扩展的主文档，按工程组成部分组织，不按日期推进。

## 目标与边界

- 复现并定制 i.MX6ULL Linux 4.9.88 Kernel、DTS 和 Buildroot 2020.02 RootFS。
- 在完成恢复能力核验后，设计并实现 Kernel + DTB + RootFS 的 A/B 系统升级。
- 保留现有应用级 OTA 与 Service Supervisor；系统 OTA 和应用 OTA 分离。
- 第一阶段不升级 U-Boot 本体。
- 所有分区写入和刷机动作必须经过恢复就绪门槛；当前仍处于只读核查和设计阶段。

## 文档导航

| 文档 | 内容 | 状态 |
|---|---|---|
| [01-platform-baseline.md](01-platform-baseline.md) | 板端硬件、eMMC、文件系统、内存基线 | 本轮 ADB 实测已记录 |
| [02-boot-chain-and-uboot.md](02-boot-chain-and-uboot.md) | U-Boot 环境、Kernel/DTB 加载路径 | printenv 已记录；MMC 探测待复核 |
| [03-ab-layout-and-recovery-constraints.md](03-ab-layout-and-recovery-constraints.md) | A/B 存储设计、容量估算和恢复门槛 | 设计分析，未实施 |
| [04-kernel-and-rootfs-customization.md](04-kernel-and-rootfs-customization.md) | Kernel、DTS、CMA 和 Buildroot 定制 | 实施计划，未实施 |
| [05-system-ota-state-machine.md](05-system-ota-state-machine.md) | 系统 OTA、健康确认和自动回滚 | 设计方案，未实施 |
| [06-recovery-readiness-checklist.md](06-recovery-readiness-checklist.md) | 恢复资料、分区、U-Boot 环境和备份检查 | 当前待办清单 |

## 当前板端事实

- Linux 4.9.88 / ARMv7 / SMP PREEMPT。
- U-Boot 2017.03。
- 当前根设备为 `/dev/mmcblk1p2`，ext4。
- eMMC user area 为 7,634,944 个 512-byte sectors，约 3.64 GiB。
- p1 = 1,000 MiB、p2 = 1,500 MiB、p3 = 500 MiB；p1/p3 用途待确认。
- p3 后约 726.97 MiB 是根据扇区几何计算出的尾部候选空间，尚未证明可安全分配。
- MemTotal = 500,948 KiB；MemAvailable = 364,744 KiB。
- CMA total = 320 MiB，采样时 CMA free 约 220.9 MiB。

## 强制安全门槛

在确认原厂镜像与恢复工具、U-Boot 本体和环境存储位置、分区表及 p1/p3 用途，并完成备份与恢复演练前：

1. 不修改分区表；
2. 不向 eMMC 写入、擦除或格式化；
3. 不执行 `saveenv` 或未经核实的 `fw_setenv`；
4. 不覆盖当前 U-Boot、Kernel、DTB 或 RootFS；
5. 不将 p1、p3 或尾部空间直接指定为 A/B 槽位。

## 设计约定

- **实测**：有板端命令输出支持。
- **已知配置**：来自当前 U-Boot 环境或已记录配置。
- **推断**：根据参数计算得到，仍需额外证据。
- **待验证**：没有足够证据，不能用于执行破坏性操作。
- **设计目标**：计划实现，尚未验收。

系统 OTA 更新非活动槽；应用 OTA 仅更新用户态 release。两者必须共用升级互斥策略，并避免对共享 DATA 的不兼容迁移。
