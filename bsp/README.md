# SmartGateway-BSP · i.MX6ULL BSP 与驱动开发

> 本目录是 [SmartGateway](../README.md) 的 BSP 学习与开发工作区，当前在 `feature/bsp-imx6ull` 分支开发。原项目 `main` 保持不变。

## 项目目标

在现有 SmartGateway Linux C / Socket / pthread 应用基础上，学习和实操 i.MX6ULL 的 Device Tree、GPIO/IRQ、I²C、用户态调试和应用级 OTA，并最后通过 UART/RS485 + Modbus RTU 扩展第二类传感器节点。仅把**有实际证据验证**的事项标记为完成。

## 当前进度（截至 D25-2 S1）

- **D01 基线调查：部分完成**。已记录硬件、U-Boot、Linux、Buildroot、eMMC 和 rootfs；原厂资料、完整恢复流程与可恢复性验证仍需完成。
- **D02 内核模块：核心实机验收通过**。Hello 模块编译、传输、加载/卸载有记录。
- **D03–D09 GPIO / Device Tree / 调试工具**：阶段文档已建立；每项状态以对应验收文档中的证据为准。
- **D10–D14 AP3216C 驱动**：I²C 读取、错误统计、故障注入与日志控制有阶段记录。
- **D15 Sensor Agent**：NODE04 / AP3216C 用户态采集和 Gateway 数据链路有实测记录。
- **D16 GDB 远程调试**：已验证 ARM 远程调试、线程/寄存器分析、NODE04 队列转发和 Gateway 端 TCP 发送成功分支；同一条消息的 Server 应用层接收确认仍按验收文档补证。
- **D17–D24 应用级 OTA**：包格式、校验、staging、版本切换、健康确认、失败回滚与故障注入均有 x86 实测记录；北向故障回滚见 D23-6-8。
- **D25-1 ARM 交叉编译：通过**。Buildroot 2020.02 SDK 工具链（glibc 2.30、OpenSSL 1.1.1g、json-c 4.0.0）产出 ARM EABI5 hard-float ELF；产物与板端库版本逐项一致。
- **D25-1B 板端实测：四项验收通过**。串口 @115200 / SSH / ADB 三通道可用；ARM Gateway 与 OTA Agent 在板端真实执行，OTA 包校验及 4 个负向用例正确；隔离启动（19000 端口）生命周期完整、无残留；`/opt` 跨重启持久（inode/md5 一致）。
  - **未验证**：Gateway 运行资源占用曾未采集，已于 D25-2 S1 阶段补齐（见 D25-1B 第 5.5 节）。
- **D25-2 进程生命周期管理：S1 / S1-R / S2 / S2-R / S2-R2 / S3 / S3-R 已完成**。
  - 设计：[D25-2 方案](docs/D25-2-process-lifecycle-design.md)
  - S1 + 两轮安全修正（pidfile 身份校验）：**145 项**单元测试
  - S2 自管 Supervisor：44 项单测 + 真实 ARM Gateway 端到端
  - S3 升级流程接入：D23 回归 **27/27** + 前置检查 **31/31**
  - S3-R 复审修正：包校验前置（损坏包不再导致业务中断）
  - S3-R2 校验补全：前置校验扩展为 **activate 的全部只读条件**，
    两者**共用同一个 `activate_check()`**，避免规则分叉
  - S3-R3 测试补强：负向用例覆盖 **7 个校验分支**（S3 前置 40 → 73 项）；
    系统化说明 PID 身份与进程停止的**保证边界**
  - 修复的真实缺陷：僵尸误判、临时文件并发、假阳性测试、
    /proc 竞争窗口、缺 boot_id、无界 waitpid、端口检查顺序陷阱、
    损坏包导致业务中断、**候选未 stage 导致业务中断**、测试误杀无关进程
  - D23 测试环境已**脚本化**（`tests/setup_ota_test_env.sh`）
  - 详见 [S1-R+S2](docs/D25-2-S1R-S2-supervisor.md)、[S2-R](docs/D25-2-S2R-review-fixes.md)、
    [S2-R2](docs/D25-2-S2R2-review-fixes.md)、[S3](docs/D25-2-S3-supervisor-integration.md)、
    [S3-R](docs/D25-2-S3R-review-fixes.md)、[S3-R2](docs/D25-2-S3R2-preflight-completeness.md)
  - **未完成**：S4（故障注入矩阵）、S5（init 集成）、S6–S7（板端完整升级）
  - **S5 关键点**：init 脚本必须写 pidfile；并应改为“经由 init 停止”，
    以从架构上移走外部停止的 TOCTOU 风险（L6）
  - **注意：本项目无 CI**，所有 PASS 均为本地/板端手工执行结果

## 下一阶段顺序（用户确认）

1. **D25-2 剩余阶段**：S4 故障注入 → S5 init 集成。
   - 已确认：板端为 **BusyBox init**，Gateway **当前无人启动且无 init 脚本**，`/opt` 持久。
   - 未确认：`start-stop-daemon` 是否存在（S5 前必须核实）。
   - **S5 关键点**：init 脚本必须写 pidfile，否则 S3 的前置检查会被绕过。
2. **OTA 完整板端验收**：x86 已完成北向故障注入与回滚验证，但**板端完整升级 / 健康检查 / 回滚流程尚未执行**，须在 D25-2 完成后补做。
3. **D26–D34：RS485 / Modbus RTU**——作为最后一个新增外设模块，完成 UART/pinmux、收发方向、CRC16、超时重试、NODE05 和多节点回归。
4. **D35–D42：综合回归与交付**——稳定性、Perf、GDB 案例、文档和候选版本。

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
- [D23-6-8 北向故障回滚验收](docs/D23-6-8-northbound-fault-rollback.md)
- [D24 TCP 连接超时与关停分析](docs/D24-tcp-connect-timeout-and-shutdown.md)
- [D25-1 ARM 交叉编译与部署验收](docs/D25-1-arm-build-deployment-validation.md)
- [D25-1B 板端实测补验收（串口/SSH、真实运行、持久化）](docs/D25-1B-board-environment-recheck.md)
- [D25-2 Gateway 进程生命周期管理设计](docs/D25-2-process-lifecycle-design.md)
- [D25-2 S1-R + S2 安全修正与自管 Supervisor](docs/D25-2-S1R-S2-supervisor.md)
- [D25-2 S2-R 复审问题修正](docs/D25-2-S2R-review-fixes.md)
- [D25-2 S2-R2 第二轮复审修正](docs/D25-2-S2R2-review-fixes.md)
- [D25-2 S3 Supervisor 接入升级流程](docs/D25-2-S3-supervisor-integration.md)
- [D25-2 S3-R 复审问题修正](docs/D25-2-S3R-review-fixes.md)
- [D25-2 S3-R2 前置校验补全](docs/D25-2-S3R2-preflight-completeness.md)
- [D25-2 S3-R3 负向测试与 PID 身份边界](docs/D25-2-S3R3-negative-tests-and-pid-boundaries.md)
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
