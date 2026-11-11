# SmartGateway

**SmartGateway 是一个以 C / Embedded Linux 为核心的多节点智能网关与 i.MX6ULL BSP 工程。**

项目围绕两条相互衔接的技术主线展开：

1. **网关应用层**：Linux C、TCP/Socket、pthread、多节点接入、协议解析、设备管理与应用级 OTA。
2. **BSP 与系统层**：i.MX6ULL 平台适配、GPIO/IRQ、I²C 驱动、Device Tree、交叉编译、进程监督，以及正在推进的 Kernel/Buildroot 定制和系统级 A/B OTA。

当前重点是第二条主线：在已经完成的 i.MX6ULL 驱动、网关和应用级 OTA 基础上，进一步掌握 Linux 启动链、存储布局、内核配置、RootFS 构建和可靠系统升级。

## 当前项目状态

| 模块 | 当前状态 |
|---|---|
| Linux Gateway / TCP Server | 已有 C / pthread / TCP / JSON Lines 实现 |
| GPIO 事件驱动 | 已完成 i.MX6ULL 板端事件链路和可靠性验证 |
| AP3216C | 已完成 I²C 驱动、错误统计、故障注入和 Sensor Agent 集成 |
| GDB 远程调试 | 已完成 ARM 远程调试实践；部分端到端接收证据仍以验收文档为准 |
| 应用级 OTA | D25-2 S1–S7 已完成，包含 Supervisor、正常升级、故障回滚和真实断电恢复 |
| BusyBox init 集成 | Gateway 开机启动及 Supervisor 崩溃后 respawn 已完成板端验证 |
| Kernel / Buildroot 定制 | 当前扩展方向，尚处于基线复核与方案准备阶段 |
| 系统级 A/B OTA | 设计与约束文档已建立，尚未进行分区改造或板端升级实现 |

**当前开发分支：** `feature/imx6ull-system-ab-ota`  
该分支基于 `feature/bsp-imx6ull` 创建，原有 BSP 实现保留在基础分支，`main` 不作为本轮直接开发分支。

> 计划、设计和历史测试记录不等于当前功能已经实现。系统级 A/B OTA 在完成恢复能力核验和存储布局评审前，不报告为 PASS。

## 平台基线

目标板：100ASK i.MX6ULL Pro。

| 项目 | 当前记录 |
|---|---|
| SoC / CPU | NXP i.MX6ULL / ARM Cortex-A7 |
| DDR | 512 MiB |
| Bootloader | U-Boot 2017.03 |
| Linux Kernel | 4.9.88，ARMv7，SMP PREEMPT |
| RootFS | Buildroot 2020.02，ext4 |
| 工具链 | Linaro GCC 6.2.1；Buildroot SDK 用于应用交叉编译 |
| eMMC user area | 7,634,944 个 512-byte sectors，约 3.64 GiB |
| 当前根分区 | `/dev/mmcblk1p2`，约 1,500 MiB |

最近一次板端采样记录了 p1/p2/p3 的扇区几何、文件系统占用、MemAvailable 和 CMA 信息。p1、p3 的实际用途、分区表类型、U-Boot 环境存储位置及可靠恢复路径仍待确认。

## 系统架构

### 网关与设备数据链路

```text
GPIO Button ──> GPIO Event Driver ──> GPIO Agent ──┐
                                                   │
AP3216C ─────> I2C Driver ──────────> Sensor Agent ├──> SmartGateway
                                                   │        │
Bluetooth / WiFi Nodes ────────────────────────────┘        │
                                                            v
                                                     TCP / JSON Lines
                                                            │
                                                            v
                                                       TCP Server
```

### Linux 启动与系统升级链路

```text
Boot ROM
   |
   v
U-Boot 2017.03
   |
   +── load Kernel (zImage)
   +── load DTB
   |
   v
Linux Kernel 4.9.88
   |
   v
ext4 RootFS (/dev/mmcblk1p2)
   |
   +── BusyBox init
   +── Service Supervisor
   +── SmartGateway / Agents
   +── Application OTA Agent
```

应用级 OTA 管理用户态 release；规划中的系统级 OTA 管理非活动系统槽中的 Kernel、DTB 和 RootFS。两种 OTA 必须保持职责分离，并通过升级互斥机制避免并发写入。

## 已完成的关键工程内容

### BSP 驱动与设备接入

- GPIO 事件字符设备、IRQ、等待队列、阻塞/非阻塞 read、poll 和事件队列。
- GPIO 端到端链路：Kernel Driver → Agent → Gateway → Server。
- Device Tree GPIO 资源化与 pinctrl 配置验证。
- AP3216C I²C 驱动、sysfs 数据接口、错误统计和故障注入。
- Sensor Agent 采集、心跳、传感器状态和 Gateway 数据上报。

### 调试与系统集成

- ARM 交叉编译与板端运行兼容性验证。
- GDB / gdbserver 远程调试。
- Service Supervisor 进程生命周期管理。
- BusyBox init 启动脚本和 inittab respawn。
- Supervisor IPC 强制接入，Supervisor 不可用时拒绝应用升级。

### 应用级 OTA

已完成包校验、staging、release 切换、健康检查、失败回滚、故障注入和板端掉电恢复。详细测试范围、命令和限制见文档索引中的 OTA / Supervisor 验收记录。

## 当前系统级扩展工作

按以下技术依赖推进，不按日期排期：

1. **平台与恢复基线**：确认 eMMC 分区表、p1/p3 用途、U-Boot 环境位置、原厂镜像匹配性和可恢复入口。
2. **Kernel 构建复现**：获取匹配的 4.9.88 源码、补丁、defconfig 和 DTS，先复现原始构建。
3. **Kernel / DTS 定制**：逐项审计驱动依赖、pinctrl、reserved-memory 和 CMA，再进行可回归的配置裁剪。
4. **Buildroot RootFS 定制**：复现 2020.02 构建配置，梳理 Gateway、Supervisor、Agent 和 OTA 运行依赖。
5. **A/B 存储与启动设计**：完成容量核算、槽位定义、Boot Control Metadata 和 U-Boot 启动选择方案。
6. **系统级 OTA**：实现非活动槽写入、镜像验证、启动健康确认、失败回滚和故障注入。

### 存储安全边界

在原厂恢复资料、U-Boot 环境存储布局、完整备份及恢复演练得到确认前：

- 不修改 eMMC 分区表；
- 不执行针对 eMMC 的 `dd` 写入、擦除或格式化；
- 不执行 `saveenv` 或未经核实的 `fw_setenv`；
- 不覆盖当前 U-Boot、Kernel、DTB 或 RootFS；
- 不将 p1、p3 或尾部候选空间直接指定为 A/B 槽位。

## 目录结构

```text
SmartGateway/
├── common/                 # 协议、帧解析、JSON 编解码
├── gateway/                # Linux Gateway 与通信 Worker
├── server/                 # TCP Server、节点状态和命令管理
├── tools/                  # Mock 节点与开发工具
├── tests/                  # 应用、OTA、Supervisor 测试
├── bsp/
│   ├── apps/               # GPIO / Sensor / OTA Agent / Supervisor
│   ├── drivers/            # BSP 驱动
│   ├── dts/                # Device Tree 实验与配置
│   ├── tools/              # BSP 辅助工具
│   └── docs/               # BSP 模块文档、参考资料和验收记录
├── README.md
├── CHANGELOG.md
└── CONTRIBUTING.md
```

## 构建与测试

主机端 Linux Gateway / Server：

```bash
make clean
make all
make test
make test-tcp
```

OTA / Supervisor 相关回归命令以对应测试脚本和验收文档为准。ARM 交叉编译需使用与目标 RootFS ABI 和动态库版本匹配的 Buildroot SDK。

本仓库当前没有 CI 工作流作为自动验收依据；文档中的 PASS 均应结合对应的本地或板端实测证据理解。

## 文档入口

- [BSP 项目说明](bsp/README.md)
- [BSP 文档总索引](bsp/docs/README.md)
- [系统定制与 A/B OTA](bsp/docs/system-ab/README.md)
- [平台与 eMMC 实测基线](bsp/docs/system-ab/01-platform-baseline.md)
- [U-Boot 启动链](bsp/docs/system-ab/02-boot-chain-and-uboot.md)
- [A/B 分区与恢复约束](bsp/docs/system-ab/03-ab-layout-and-recovery-constraints.md)
- [Kernel / RootFS 定制](bsp/docs/system-ab/04-kernel-and-rootfs-customization.md)
- [系统 OTA 状态机](bsp/docs/system-ab/05-system-ota-state-machine.md)

历史提交和版本变化见 [CHANGELOG.md](CHANGELOG.md)；提交规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。
