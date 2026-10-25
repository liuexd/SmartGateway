# SmartGateway-BSP · i.MX6ULL BSP 与驱动开发

> 本目录是 [SmartGateway](../README.md) 的 BSP 学习与开发工作区，当前在 `feature/bsp-imx6ull` 分支开发。原项目 `main` 保持不变。

## 项目目标

在现有 SmartGateway Linux C / Socket / pthread 应用基础上，学习和实操 i.MX6ULL 的 Device Tree、GPIO/IRQ、I²C、用户态调试和应用级 OTA，并最后通过 UART/RS485 + Modbus RTU 扩展第二类传感器节点。仅把**有实际证据验证**的事项标记为完成。

## 当前进度（截至 D16）

- **D01 基线调查：部分完成**。已记录硬件、U-Boot、Linux、Buildroot、eMMC 和 rootfs；原厂资料、完整恢复流程与可恢复性验证仍需完成。
- **D02 内核模块：核心实机验收通过**。Hello 模块编译、传输、加载/卸载有记录。
- **D03–D09 GPIO / Device Tree / 调试工具**：阶段文档已建立；每项状态以对应验收文档中的证据为准。
- **D10–D14 AP3216C 驱动**：I²C 读取、错误统计、故障注入与日志控制有阶段记录。
- **D15 Sensor Agent**：NODE04 / AP3216C 用户态采集和 Gateway 数据链路有实测记录。
- **D16 GDB 远程调试**：已验证 ARM 远程调试、线程/寄存器分析、NODE04 队列转发和 Gateway 端 TCP 发送成功分支；同一条消息的 Server 应用层接收确认仍按验收文档补证。

## 下一阶段顺序（用户确认）

1. **D17–D25：应用级 OTA**——先核查存储与恢复条件，再实现升级包校验、staging、版本切换、健康确认、失败回滚和故障注入。
2. **D26–D34：RS485 / Modbus RTU**——作为最后一个新增外设模块，完成 UART/pinmux、收发方向、CRC16、超时重试、NODE05 和多节点回归。
3. **D35–D42：综合回归与交付**——稳定性、Perf、GDB 案例、文档和候选版本。

详细任务、验收标准和安全边界见 [D16 后路线图](docs/ROADMAP.md)。

## 文档导航

### 阶段验收

- [D01 系统基线](docs/D01-baseline.md)
- [D01 原厂资料与恢复核对清单](docs/D01-recovery-checklist.md)
- [D01 Ubuntu 构建环境与 Git 工作区](docs/D01-host-environment.md)
- [D02 内核模块实机验收](docs/D02-kernel-module.md)
- [D06 GPIO 端到端验收](docs/D06-gpio-end-to-end-validation.md)
- [D07 GPIO 可靠性验收](docs/D07-gpio-reliability-validation.md)
- [D08 Device Tree GPIO 验收](docs/D08-device-tree-gpio-validation.md)
- [D09 GDB / Perf 验收](docs/D09-gdb-perf-validation.md)
- [D10–D14 AP3216C 驱动与异常处理](docs/D10-ap3216-driver.md)
- [D15 AP3216C Sensor Agent 集成](docs/D15-ap3216c-sensor-agent-integration.md)
- [D16 GDB 远程调试验收](docs/D16-gdb-remote-debugging.md)
- [GDB/gdbserver 零基础实战教程](docs/tutorial-gdbserver-arm-remote-debugging.md)
- [D16 后调整路线图：OTA 优先，RS485 最后](docs/ROADMAP.md)

### 硬件与软件基线（已记录）

| 项目 | 值 |
| --- | --- |
| SoC | NXP / Freescale i.MX6ULL rev1.1，单核 Cortex-A7 |
| Device Tree model | Freescale i.MX6 ULL 14x14 EVK Board（系统上报的 DT model，物理 PCB 型号尚需核对） |
| DDR | 512 MiB |
| 存储 | eMMC S40004，约 3.64 GiB |
| Bootloader | U-Boot 2017.03-g8ba4c5bb19-dirty |
| Kernel | Linux 4.9.88，ARMv7 |
| Rootfs | Buildroot 2020.02（2020.02-g65177d4） |
| 根文件系统 | `/dev/mmcblk1p2`，ext4（安装目录持久性仍须在 D17 核验） |
| 内核控制台 | `ttymxc0,115200` |
| 调试通道 | 板载 USB 转串口可捕获 U-Boot 日志；ADB Shell 可进入系统 |

## Git 与阶段验收规则

- 所有 BSP 工作在 `feature/bsp-imx6ull` 分支进行，不在 `main` 直接开发。
- 按 `docs(bsp)`、`build(bsp)`、`feat(driver)`、`test(bsp)` 等 Conventional Commits 形式提交，每个提交聚焦一个主题。
- 每阶段保留命令、实测输出、验收结果与已知问题；计划中的工作不得写成 PASS。
- 原厂恢复介质与可靠恢复方式未确认前，不执行 `dd` 写块设备、`mmc write`、`saveenv`、刷写 U-Boot 或覆盖现有 DTB / Kernel。
- 不把原厂镜像、工具链或私人设备资料上传到公开仓库。
