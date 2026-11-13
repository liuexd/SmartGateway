# BSP Documentation Index

本目录按**工程模块**组织，不再按 D01、D02、D03 的时间顺序组织。阶段编号仅保留在历史验收文档正文中，用于追溯测试过程。

## 当前开发文档

### System customization and A/B OTA

`system-ab/` 是当前开发主线，覆盖平台基线、U-Boot、存储布局、Kernel/RootFS 定制和系统 OTA。

- [系统定制与 A/B OTA 总览](system-ab/README.md)
- [平台与 eMMC 基线](system-ab/01-platform-baseline.md)
- [U-Boot 启动链与环境变量](system-ab/02-boot-chain-and-uboot.md)
- [A/B 分区布局与恢复约束](system-ab/03-ab-layout-and-recovery-constraints.md)
- [Kernel 与 Buildroot RootFS 定制](system-ab/04-kernel-and-rootfs-customization.md)
- [系统 OTA 状态机](system-ab/05-system-ota-state-machine.md)
- [恢复就绪检查表](system-ab/06-recovery-readiness-checklist.md)
- [G1：恢复基线与工具链核查](system-ab/07-g1-recovery-baseline-and-toolchain.md)
- [G2：U-Boot A/B 启动管理与 Watchdog](system-ab/08-g2-uboot-ab-boot-and-watchdog.md)
- [G1 主机验收报告](validation/platform/G1-baseline-and-recovery-prep.md)
- [G2 U-Boot A/B 主机验收报告](validation/ota/G2-uboot-ab-host-validation.md)

## 技术参考

`reference/` 保存可重复查阅的知识和环境说明，不代表新的验收任务。

| 分类 | 文档 |
|---|---|
| G0 inventory | [Read-only eMMC inventory script](../tools/collect_system_baseline.sh) | Collects partition, filesystem and boot metadata without writing eMMC |
| G0-1B result | [p1/p3 and RTC follow-up](validation/platform/G0-1B-p1-p3-rtc-followup.md) | p1/p3 first 2048 bytes zero; RTC reports 1970; no repartition authorization |
| G0-1 result | [eMMC inventory findings](validation/platform/G0-1-emmc-inventory.md) | DOS/MBR confirmed; p1/p3 contents and purpose remain unknown |
| Build | [Host build environment](reference/build/host-environment.md) |
| Kernel / Driver | [Platform driver](reference/kernel-driver/platform-driver.md) |
| Kernel / Driver | [Device Tree](reference/kernel-driver/device-tree.md) |
| Kernel / Driver | [Platform resources and devm](reference/kernel-driver/platform-resources-and-devm.md) |
| Debug | [ARM gdbserver remote debugging tutorial](reference/debug/gdbserver-arm-remote-debugging.md) |

## 历史验收记录

`validation/` 保存已经执行过的阶段验证。报告中的 PASS 只适用于文档记录的测试环境、版本和测试范围；不能自动推导为当前分支的新功能已经完成。

| 分类 | 内容 |
|---|---|
| [Platform](validation/platform/) | 初始板端基线、板端环境复核 |
| [Kernel](validation/kernel/) | Hello 内核模块构建与加载 |
| [GPIO](validation/gpio/) | GPIO 端到端、可靠性、Device Tree 资源验证 |
| [Sensors](validation/sensors/ap3216c/) | AP3216C 驱动、I²C 错误处理、故障注入和 Agent 集成 |
| [Debug](validation/debug/) | GDB / Perf 阶段记录、ARM 远程调试验收 |
| [Build](validation/build/) | ARM 交叉编译与部署 |
| [Application OTA](validation/ota/) | 应用升级、回滚、TCP 超时和板端环境验证 |
| [Supervisor](validation/ota/supervisor/) | 进程生命周期、S1–S7、故障注入和板端恢复 |

## 历史资料

[archive/](archive/) 仅保存被新基线替代的旧检查表和历史快照，不作为当前任务清单。

## 维护规则

1. 新的设计文档放入对应技术模块，不以日期作为目录主轴。
2. 新的验收报告放入 `validation/<module>/`，记录硬件、软件版本、命令、原始结果、判据和限制。
3. 计划、设计、实现、验证四种状态必须明确区分。
4. 已废弃路线图不继续留在当前文档入口；历史测试证据不因阶段结束而删除。
5. 修改分区表、U-Boot 环境或系统镜像前，必须先满足恢复就绪门槛。
