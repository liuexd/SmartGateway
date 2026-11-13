# i.MX6ULL System Customization and A/B OTA

本目录是当前系统级 BSP 扩展的主文档，按工程组成部分组织，不按日期推进。

## 当前证据与实施规划入口

[当前前置条件与 G1–G6 分阶段规划](readiness-and-roadmap/README.md) 汇总截至 2026-11-13 的已核实事实、证据边界和未证实事项，并细化恢复验证、p2 保护、槽位容量、bootcount 后端、健康检查及掉电验收。该独立主题补充下列历史核查入口；G0 仍未通过，文档提交不构成写板放行。

## 目标与边界

- 复现并定制 i.MX6ULL Linux 4.9.88 Kernel、DTS 和 Buildroot 2020.02 RootFS。
- 在完成恢复能力核验后，设计并实现 Kernel + DTB + RootFS 的 A/B 系统升级。
- 保留现有应用级 OTA 与 Service Supervisor；系统 OTA 和应用 OTA 分离。
- 第一阶段不升级 U-Boot 本体。
- 所有分区写入和刷机动作必须经过恢复就绪门槛；当前仍处于只读核查和设计阶段。

## G0-1：当前执行入口

先在目标板运行只读采集脚本：

```sh
# 将脚本从仓库复制到板端 /tmp 后执行
sh /tmp/collect_system_baseline.sh
# 报告位置
cat /tmp/sgw-g0-readonly-inventory.txt
```

脚本位于 [bsp/tools/collect_system_baseline.sh](../../tools/collect_system_baseline.sh)。它不挂载分区、不运行修复、不修改 U-Boot 环境，也不向 eMMC 写入。工具缺失会明确记录，不要为了补齐工具而先改造当前 RootFS。脚本有意不输出 eMMC serial/CID 等设备唯一标识；公开分享报告前仍应检查是否含有个人路径、网络信息或其他敏感内容。

**本次只需回传报告内容。不要执行分区创建、格式化、dd 写入、saveenv 或 fw_setenv。**

## 文档导航

| 文档 | 内容 | 状态 |
|---|---|---|
| [01-platform-baseline.md](01-platform-baseline.md) | 板端硬件、eMMC、文件系统、内存基线 | 本轮 ADB 实测已记录 |
| [02-boot-chain-and-uboot.md](02-boot-chain-and-uboot.md) | U-Boot 环境、Kernel/DTB 加载路径 | printenv 已记录；MMC 探测待复核 |
| [03-ab-layout-and-recovery-constraints.md](03-ab-layout-and-recovery-constraints.md) | A/B 存储设计、容量估算和恢复门槛 | 设计分析，未实施 |
| [04-kernel-and-rootfs-customization.md](04-kernel-and-rootfs-customization.md) | Kernel、DTS、CMA 和 Buildroot 定制 | 实施计划，未实施 |
| [05-system-ota-state-machine.md](05-system-ota-state-machine.md) | 系统 OTA、健康确认和自动回滚 | 设计方案，未实施 |
| [06-recovery-readiness-checklist.md](06-recovery-readiness-checklist.md) | 恢复资料、分区、U-Boot 环境和备份检查 | 当前待办清单 |
| [07-g1-recovery-baseline-and-toolchain.md](07-g1-recovery-baseline-and-toolchain.md) | G1 恢复基线、分支、工具链核查与只读备份方案 | HOST-VERIFIED；板端演练待执行 |
| [08-g2-uboot-ab-boot-and-watchdog.md](08-g2-uboot-ab-boot-and-watchdog.md) | G2 U-Boot A/B 状态机、bootcount 持久化、冗余 env 与 Watchdog | HOST-TESTED；未写板 |

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
