# SmartGateway-BSP

**面向 100ASK i.MX6ULL Pro 的 Linux BSP、驱动开发与系统升级工程。**

本目录承接 SmartGateway 的 Linux 用户态网关应用，向下覆盖 i.MX6ULL 平台、Kernel、Device Tree、GPIO/I²C 驱动和系统启动；向上覆盖 Agent、Gateway、Service Supervisor 与应用级 OTA。

当前开发重点已从单个外设驱动和应用 OTA，转向 **Kernel / Buildroot 定制与系统级 A/B OTA**。

## 1. 开发分支与项目边界

| 分支 | 职责 |
|---|---|
| `feature/bsp-imx6ull` | 已完成的 i.MX6ULL BSP 驱动、Agent、调试和应用级 OTA 基线 |
| `feature/imx6ull-system-ab-ota` | 当前系统定制、存储布局、A/B 启动和系统 OTA 扩展 |
| `main` | 保持原项目主线，不直接进行本轮开发 |

当前分支基于 `feature/bsp-imx6ull` 创建。系统级扩展不替代原有应用级 OTA，也不把历史验收记录当作新功能实现。

## 2. 项目目标

围绕真实 i.MX6ULL 板卡，完成可复现、可解释、可故障验证的 BSP 工程：

- 理解 U-Boot → Kernel / DTB → RootFS 的启动链。
- 掌握设备树、pinctrl、GPIO、IRQ、I²C 和平台驱动。
- 建立 Kernel 与 Buildroot 的可复现构建环境。
- 完成 GPIO 事件与 AP3216C 传感器的驱动到用户态数据链路。
- 使用 GDB / gdbserver 定位 ARM 用户态问题。
- 使用 Supervisor 和 BusyBox init 管理 Gateway 生命周期。
- 保持应用级 OTA 的版本切换、健康检查和回滚能力。
- 在安全恢复前提下，进一步实现 Kernel、DTB、RootFS 的系统级 A/B OTA。

## 3. 当前技术架构

```text
                         TCP Server
                              ^
                              |
                         SmartGateway
                              ^
                 +------------+------------+
                 |                         |
             GPIO Agent                Sensor Agent
                 ^                         ^
                 |                         |
          GPIO Event Driver            AP3216C Driver
                 ^                         ^
                 +------ i.MX6ULL ---------+
                            |
                     Linux Kernel 4.9.88
                            |
                       ext4 RootFS
                            |
                       BusyBox init
                            |
                    Service Supervisor
                            |
                    Application OTA Agent
```

系统启动链：

```text
Boot ROM -> U-Boot -> zImage + DTB -> Linux Kernel -> ext4 RootFS
                                                   -> BusyBox init
                                                   -> Supervisor / Gateway / Agents
```

OTA 职责边界：

| 类型 | 更新对象 | 当前状态 |
|---|---|---|
| 应用级 OTA | SmartGateway 用户态 release | 已完成主要升级、健康检查、回滚与掉电恢复验证 |
| 系统级 OTA | 非活动槽中的 Kernel、DTB、RootFS | 当前设计阶段，尚未板端实现 |
| Bootloader 更新 | U-Boot 本体及环境 | 不在第一阶段升级范围 |

## 4. 已完成能力与证据边界

| 模块 | 当前结论 |
|---|---|
| 内核模块 | Hello 模块交叉编译、传输、加载和卸载有板端记录 |
| GPIO / IRQ | 事件字符设备、read、poll、非阻塞和端到端上报有实测记录 |
| Device Tree | GPIO pinctrl、资源描述和板端验证有记录 |
| AP3216C | I²C 读取、sysfs、错误统计、故障注入和 Sensor Agent 集成有记录 |
| GDB | ARM gdbserver 远程调试有实测记录 |
| Perf | 已有阶段性调试记录，具体覆盖范围见验收文档 |
| 应用级 OTA | D25-2 S1–S7 完成；包括故障注入、板端升级、回滚和真实断电恢复 |
| Service Supervisor | 生命周期、IPC、崩溃检测、SIGKILL、陈旧 socket 和重启持久性有实测记录 |
| BusyBox init | Gateway 开机启动和 Supervisor respawn 已完成板端验证 |
| 系统级 A/B OTA | 仅完成平台基线和设计约束文档，未修改分区、未实现槽位切换 |

注意：GDB 远程调试的 Server 应用层接收确认、真实云端链路、串口业务模式、eMMC 坏块/只读故障等仍有未覆盖项，具体以验证记录为准。

## 5. 板端平台基线

| 项目 | 实测 / 已记录值 |
|---|---|
| 开发板 | 100ASK i.MX6ULL Pro |
| SoC | i.MX6ULL |
| CPU 架构 | ARMv7 / Cortex-A7 |
| DDR | 512 MiB |
| U-Boot | 2017.03 |
| Linux | 4.9.88，SMP PREEMPT |
| RootFS | Buildroot 2020.02，ext4 |
| Kernel 编译器 | Linaro GCC 6.2.1 |
| 根设备 | `/dev/mmcblk1p2` |

最近一次 ADB 实测：

| 项目 | 结果 |
|---|---:|
| eMMC user area | 7,634,944 × 512-byte sectors，约 3.64 GiB |
| p1 | 1,000 MiB，实际用途待确认 |
| p2 | 1,500 MiB，当前根分区 |
| p3 | 500 MiB，实际用途待确认 |
| p2 文件系统 | 677 MiB used / 706 MiB available |
| p3 后剩余区域 | 算术推算约 726.97 MiB，尚未证明可安全使用 |
| MemTotal | 500,948 KiB |
| MemAvailable | 364,744 KiB |
| CMA | 320 MiB total / 约 220.9 MiB free（采样时点） |

U-Boot 环境变量显示当前从 eMMC 的 mmc 1:2 加载 `/boot/zImage` 和 DTB，Linux 根设备为 `/dev/mmcblk1p2`。U-Boot 环境实际存储介质、偏移、冗余方式，以及 p1/p3 用途仍未确认。

## 6. 当前工作顺序

不按 D26、D27 等日期推进，按技术依赖和安全门槛推进：

1. **平台与恢复核查**：确认分区表、p1/p3 内容、U-Boot 环境存储位置、原厂镜像匹配性和恢复入口。
2. **Kernel 构建复现**：获取匹配的 4.9.88 源码、补丁、defconfig、DTS 和工具链，先复现原始镜像。
3. **Kernel / DTS 定制**：审计驱动依赖、pinctrl、reserved-memory、CMA 和启动参数，再做逐项裁剪。
4. **Buildroot RootFS 定制**：复现 2020.02 配置，梳理 BusyBox init、Supervisor、Gateway、Agent 和 OTA 依赖。
5. **A/B 存储布局**：根据真实容量、原厂保留区和恢复方案确定槽位、DATA 与 Boot Control Metadata。
6. **系统 OTA**：实现非活动槽写入、校验、pending 启动、健康确认、失败回滚和断电故障注入。
7. **性能与调试复盘**：在功能闭环之后，用 GDB / Perf 形成可量化的问题定位或优化案例。

## 7. 存储与刷写安全门槛

在恢复能力核验完成之前，禁止：

- 修改 eMMC 分区表；
- 对 `/dev/mmcblk1` 或其分区执行未经验证的写入、擦除或格式化；
- 执行 `saveenv` 或使用未经核实的 `fw_env.config`；
- 覆盖当前 U-Boot、Kernel、DTB 和 RootFS；
- 将 p1、p3 或 p3 后尾部区域直接定义为 A/B 槽位。

当前 U-Boot 曾出现 `mmc part` 报告 `no card present` / `mmc_init: -123`，但 Linux 能识别 eMMC。需要先按只读方式复核 `mmc dev 1`、`mmc rescan`、`mmc info` 和 `mmc part`，不能跳过异常直接设计写入流程。

## 8. 当前只读核查工具

G0-1 使用 [`bsp/tools/collect_system_baseline.sh`](tools/collect_system_baseline.sh) 采集 eMMC 分区表、p1/p2/p3 文件系统签名、挂载情况和相关内核日志。脚本只写入 `/tmp/sgw-g0-readonly-inventory.txt`，不写 eMMC。执行前将脚本复制到板端 `/tmp`；执行后把报告回传复核。

## 9. 文档导航

- [BSP 文档总索引](docs/README.md)
- [系统定制与 A/B OTA](docs/system-ab/README.md)
- [平台与 eMMC 基线](docs/system-ab/01-platform-baseline.md)
- [U-Boot 启动链](docs/system-ab/02-boot-chain-and-uboot.md)
- [A/B 分区与恢复约束](docs/system-ab/03-ab-layout-and-recovery-constraints.md)
- [Kernel / RootFS 定制](docs/system-ab/04-kernel-and-rootfs-customization.md)
- [系统 OTA 状态机](docs/system-ab/05-system-ota-state-machine.md)
- [恢复就绪检查表](docs/system-ab/06-recovery-readiness-checklist.md)

历史验收报告按模块存放于 `docs/validation/`；长期参考资料存放于 `docs/reference/`。历史阶段编号保留在文档正文中，以便追溯原始测试记录。
