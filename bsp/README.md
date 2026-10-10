# SmartGateway-BSP · i.MX6ULL BSP 与驱动开发

> 本目录是 [SmartGateway](../README.md) 的 BSP 学习与开发工作区，当前在 `feature/bsp-imx6ull` 分支开发。原项目 `main` 保持不变。

## 项目目标

在现有 SmartGateway 的 Linux C / Socket / pthread 应用基础上，系统学习并实操 i.MX6ULL 的 U-Boot、Linux Kernel 4.9.88、Device Tree、Buildroot、字符设备 / Platform / GPIO / IRQ 驱动，并最终将板端驱动事件接入 SmartGateway。仅把**实际完成且经过验证**的事项标记为完成。

## 当前状态

- **D01：系统基线调查（部分完成）**：板端硬件、Kernel、eMMC、rootfs、U-Boot 启动阶段已核对。
- **D01 尚待完成**：核对百问网原厂资料包、恢复介质与刷机流程；保存一份完整且可复现的启动日志；完成恢复可行性验证（以安全的非破坏性检查为先）。
- **D02：核心实机验收已通过**：使用既有 Linux 4.9.88 内核构建树和 Linaro GCC 6.2.1 编译 `bsp_hello.ko`，ADB 传输及 SHA-256 校验一致，开发板 `insmod` / `rmmod` 均有对应初始化与退出日志。卸载后模块列表检查、错误上传路径残留核对待补。
- **D03：准备开始**：先进行 Platform Driver 框架及设备树的只读分析。D01 原厂恢复验收仍为独立待办，未经验证不刷写系统镜像。
- 仓库根目录现有网关源码不作更改；本目录有已编译并在实机加载验证的 **Hello 内核模块**，但这不是完整的 BSP 移植或硬件驱动功能验收。

## 文档导航

- [D01 实测基线](docs/D01-baseline.md)：已确认的信息、启动过程和待核对事项。
- [D01 原厂资料与恢复核对清单](docs/D01-recovery-checklist.md)：资料包清点、恢复条件、禁止误操作。
- [D01 Ubuntu 构建环境与 Git 工作区](docs/D01-host-environment.md)：内核构建树、交叉工具链、本地独立克隆记录。
- [D02 内核模块实机验收](docs/D02-kernel-module.md)：编译、ADB、SHA-256、insmod/rmmod 实测证据和待补项。
- [U-Boot 启动日志摘录](logs/D01-uboot-excerpt.log)：用户通过板载 USB 调试串口截取的输出，非完整原始日志。
- [Kernel 启动日志摘录](logs/D01-kernel-excerpt.log)：板端 `dmesg` 的重点行，非完整原始日志。

## 硬件与软件基线（已核对）

| 项目 | 值 |
| --- | --- |
| SoC | NXP / Freescale i.MX6ULL rev1.1，单核 Cortex-A7 |
| Device Tree model | Freescale i.MX6 ULL 14x14 EVK Board（这是系统上报的 DT model，物理 PCB 型号尚需核对） |
| DDR | 512 MiB |
| 存储 | eMMC S40004，约 3.64 GiB |
| Bootloader | U-Boot 2017.03-g8ba4c5bb19-dirty |
| Kernel | Linux 4.9.88，ARMv7 |
| Rootfs | Buildroot 2020.02（2020.02-g65177d4） |
| 根文件系统 | `/dev/mmcblk1p2`，ext4 |
| 内核控制台 | `ttymxc0,115200` |
| 调试通道 | 板载 USB 转串口已能捕获 U-Boot 日志；ADB Shell 也可进入系统 |

## D01 已执行的只读检查

```sh
uname -a
cat /etc/os-release
tr -d '\0' < /proc/device-tree/model; echo
tr '\0' '\n' < /proc/device-tree/compatible
cat /proc/cmdline
cat /proc/partitions
cat /proc/mounts
cat /sys/block/mmcblk1/device/type
cat /sys/block/mmcblk1/device/name
dmesg | head -n 60
dmesg | grep -Ei 'mmc|EXT4-fs|Linux version|Machine model'
```

## 分支与 Commit 规则

- 新工作从 `feature/bsp-imx6ull` 分支进行，不在 `main` 上直接开发 BSP。
- 按 `docs(bsp)` / `build(bsp)` / `feat(driver)` / `test(bsp)` 等 Conventional Commits 形式提交，保持一次 Commit 一个可解释的主题。
- 每个阶段提供命令、实际输出、验收结果与已知问题；不要把未经验证的操作记成 PASS。
- 未来要合并至 `main` 时，再单独提 Pull Request 审核。

**安全规则：** 在确认厂商 BSP、存储映像、启动介质与可靠恢复方式之前，不执行 `dd` 写块设备、`mmc write`、`saveenv`、烧录 U-Boot 或覆盖现有 DTB / Kernel。原厂镜像、工具链和私人设备资料不要直接上传到公开仓库。
