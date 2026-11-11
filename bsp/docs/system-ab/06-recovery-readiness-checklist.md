# Recovery Readiness Checklist

**用途：** 在 i.MX6ULL eMMC 分区改造、U-Boot 环境修改或系统级 OTA 实施前，确认设备具有可验证的恢复路径。

**当前状态：** 未达到 G0；当前只允许只读采集和构建环境准备。

## 1. 已确认的板端基线

- [x] Linux 版本：4.9.88，ARMv7，SMP PREEMPT。
- [x] U-Boot 版本：2017.03（版本信息已记录）。
- [x] 当前 Linux 根设备：`/dev/mmcblk1p2`，ext4。
- [x] eMMC user area 总容量：7,634,944 个 512-byte sectors。
- [x] p1/p2/p3 的 Linux 可见 start/size 已记录。
- [x] 当前 U-Boot `printenv` 已保存，包含 `mmcdev=1`、`mmcpart=2`、`mmcroot=/dev/mmcblk1p2 rootwait rw`。
- [x] 当前根文件系统使用量已记录：677 MiB used、706 MiB available。
- [x] 内存采样已记录：MemTotal 500,948 KiB、MemAvailable 364,744 KiB、CmaTotal 327,680 KiB。

以上仅表示信息已采集，不表示设备已具备恢复能力。

## 2. 仍需完成的只读核查

### 板卡与原厂资料

- [ ] 核对 PCB 丝印、硬件版本和准确商品型号。
- [ ] 获取与当前板型匹配的原理图、硬件手册、启动模式和恢复说明。
- [ ] 获取与板端 U-Boot 2017.03 对应的源码、补丁、defconfig 和 .config。
- [ ] 获取与板端 Linux 4.9.88 对应的源码、补丁、defconfig 和 DTS。
- [ ] 获取 Buildroot 2020.02 的实际配置、外部工具链和 RootFS 构建说明。
- [ ] 对原厂候选镜像逐一核对适用板型、版本、大小、来源和 SHA-256；不能只凭文件名判断匹配。
- [ ] 确认原厂恢复工具版本、恢复介质、供电要求和完整恢复步骤。

### eMMC 与分区

- [x] 确认当前根分区为 p2。
- [x] 记录 p1：start 4098，size 2,048,000 sectors。
- [x] 记录 p2：start 2,052,098，size 3,072,000 sectors。
- [x] 记录 p3：start 5,124,098，size 1,024,000 sectors。
- [x] 使用只读 `fdisk -l /dev/mmcblk1` 确认分区表类型为 DOS/MBR；原始分区表导出/备份仍待完成。
- [ ] 识别 p1、p3 的实际文件系统、标签、UUID、PARTUUID 和内容；当前 `blkid` 无识别输出，前 2048 bytes 全零，但整个分区尚未扫描。
- [ ] 确认 p1、p3 是否包含启动文件、厂商恢复数据或其他保留数据。
- [x] 确认 MBR 类型 ID：p1/p2=0x83，p3=0x0C；该字段不等同于内部文件系统类型。
- [x] 确认 p1/p2 的 MBR bootable 标志均置位；其是否被 U-Boot 使用仍待查。
- [ ] 通过原厂分区布局和分区表交叉确认 p3 后的尾部空间是否可分配。
- [x] 记录非唯一设备字段：type=MMC、name=S40004、manfid=0x000001、oemid=0x0100、date=06/2020 及容量；serial/CID 未写入公开记录以保护设备唯一标识。CSD 如后续确有必要，在本地私有记录中核查。

**容量提醒：** p3 后按扇区算术推算剩余 1,486,846 sectors，约 726.97 MiB。这只是 Linux 暴露几何数据的计算结果，不证明该区域可安全使用。

### U-Boot 与启动配置

- [x] 记录 `bootcmd`、`mmcdev`、`mmcpart`、`mmcroot`、Kernel/DTB 文件名和加载地址。
- [x] 记录当前 Linux cmdline：`root=/dev/mmcblk1p2 rootwait rw`。
- [ ] 在 U-Boot 只读执行 `mmc dev 1`、`mmc rescan`、`mmc info`、`mmc part` 并保存完整日志；Linux 侧已确认 eMMC 正常枚举，不能据此推定 U-Boot 侧也正常。
- [ ] 查明 U-Boot 本体存放于 eMMC user area、boot0、boot1 或其他位置。
- [ ] 从匹配的 U-Boot 配置和源码确认环境变量存储介质、offset、size、sector size。
- [ ] 确认是否启用 redundant environment，以及掉电时的恢复语义。
- [ ] 确认是否有 bootcount / bootlimit 能力；不能仅凭 `help bootcount` 无命令判断配置不存在。
- [ ] 确认原厂工具是否能够恢复 U-Boot 本体及环境变量。

**已知异常：** 当前记录中 U-Boot `mmc list` 可列出 eMMC 控制器，但随后 `mmc part` 曾返回 `MMC: no card present` / `mmc_init: -123`。Linux 同时能够识别 eMMC。该异常必须先复核，不能直接推断为介质损坏或分区表损坏。

### G0-1B 补充结论

- [x] p1/p3 的 `blkid -p -o full` 无识别输出。
- [x] p1/p3 前 2048 bytes 全零。
- [x] RTC 与系统时间显示 1970-01-06，当前日志墙钟时间不可信。
- [x] 发现 fstab 声明 ext2/noauto 与实际 ext4 根挂载不一致，用户空间挂载流程待追踪。
- [ ] 不得据此认定 p1/p3 全盘为空或可回收。

### 本轮 G0-1 结论

- [x] Linux 侧识别 eMMC user area 和 p1/p2/p3；容量及起始扇区与前次记录一致。
- [x] 只读 fdisk 确认 DOS/MBR。
- [x] 当前根文件系统为 p2/ext4；p1/p3 未挂载。
- [ ] p1/p3 文件系统与内容未识别，禁止将其作为 A/B 槽或空闲空间。
- [ ] p3 后约 726.97 MiB 仍只是几何推算，不能分配。
- [ ] 板端报告时间为 1970-01-06，需查明 RTC/NTP 时间同步；日志时间暂不可信。

## 3. 备份与恢复演练

以下步骤只有在明确原厂流程、读写范围和供电风险后才可执行：

- [ ] 制定 eMMC user area 备份方案，确认备份覆盖范围和读取方式。
- [ ] 评估是否需要备份 boot0/boot1；明确访问保护、写保护和风险。
- [ ] 保存分区表、U-Boot 环境、启动文件、DTB、Kernel 和 RootFS 的备份及 SHA-256。
- [ ] 将备份保存在开发板以外的可靠介质，不只保存在被测 eMMC。
- [ ] 验证备份文件大小、校验值和可读取性。
- [ ] 确认可从不依赖当前 RootFS 的介质进入恢复模式。
- [ ] 在不会破坏当前工作系统的条件下完成恢复演练或等价的厂商流程验证。
- [ ] 恢复后验证串口启动、Kernel、RootFS、网络、ADB、GPIO、I²C、Gateway 和 Supervisor。

## 4. G0 放行条件

以下项目全部完成并留有证据，才允许进入分区布局变更评审：

- [ ] 原厂镜像、工具和板型匹配性得到确认。
- [ ] U-Boot 本体和环境变量的物理存储布局得到确认。
- [ ] p1、p3 和尾部空间用途得到确认。
- [ ] 完整备份已完成并通过校验。
- [ ] 恢复入口和恢复步骤可用。
- [ ] 恢复演练或可信等价验证通过。
- [ ] 恢复后核心业务功能回归通过。

**G0 未通过：** 仅允许只读检查、主机端编译、文档完善和不触碰现有启动介质的实验。不得分区、刷写、擦除、格式化或保存 U-Boot 环境。

## 5. 后续证据归档

每次核查至少保存：
- 执行环境与板卡型号；
- 完整命令和原始输出；
- 工具版本；
- 镜像来源、文件大小和 SHA-256；
- 结论及未解决问题。

公开仓库只提交不含个人路径、账号、设备唯一标识、密钥或私有镜像的必要信息。
