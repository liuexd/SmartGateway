# i.MX6ULL System Customization and A/B OTA

本目录按工程组成部分组织系统级 BSP 扩展资料，不按 D26、D27 等日期顺序组织。文档区分实测、已知配置、推断、待验证和设计目标；未经板端验证的事项不得标记 PASS。

## 文档导航

| 文档 | 内容 | 状态 |
|---|---|---|
| 01-platform-baseline.md | 板端硬件、eMMC 分区、文件系统、内存基线 | 本轮 ADB 实测已记录 |
| 02-boot-chain-and-uboot.md | U-Boot 环境、Kernel/DTB 加载路径、启动参数 | printenv 已记录；MMC 交互探测待复核 |
| 03-ab-layout-and-recovery-constraints.md | A/B 分区设计约束、容量估算、恢复门槛 | 设计分析，未实施 |
| 04-kernel-and-rootfs-customization.md | Kernel 裁剪、DTS/CMA、Buildroot 定制 | 实施计划，未实施 |
| 05-system-ota-state-machine.md | 系统 OTA、启动确认、失败回滚状态机 | 实施计划，未实施 |

## 当前工程边界

- 原有 GPIO、AP3216C、Gateway、Supervisor 和应用级 OTA 属于既有基线，不在此重复实现。
- 本分支新增目标为 Kernel/RootFS 定制，以及 Kernel、DTB、RootFS 的系统级 A/B OTA。
- 第一阶段不升级 U-Boot 本体；Bootloader 保持单份，必须先验证可恢复性。
- 当前 eMMC p1、p3 的用途尚未确认；未分配尾部空间尚未通过分区表工具和原厂资料交叉核实。
- 分区图和状态机均为逻辑设计，不是可直接执行的刷写指令。

## 强制安全门槛

在完成原厂恢复介质核验、U-Boot 环境存储布局确认、完整 eMMC 备份及恢复演练前，不修改分区表、不对 eMMC 执行写入/擦除/格式化、不执行 saveenv/fw_setenv、不覆盖当前 Kernel、DTB、RootFS 或 U-Boot，也不将 p1、p3 或未分配空间擅自指定为 A/B 槽位。
