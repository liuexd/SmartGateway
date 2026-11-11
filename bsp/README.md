# SmartGateway-BSP · i.MX6ULL BSP 与驱动开发

> 本目录是 [SmartGateway](../README.md) 的 BSP 学习与开发工作区。原有驱动与应用 OTA 基线位于 `feature/bsp-imx6ull`；系统定制与 A/B OTA 扩展位于 `feature/imx6ull-system-ab-ota`。原项目 `main` 保持不变。

## 项目目标

在现有 SmartGateway Linux C / Socket / pthread 应用基础上，完成 i.MX6ULL Device Tree、GPIO/IRQ、I²C、用户态调试、应用级 OTA 与 Supervisor；后续扩展方向调整为 Linux Kernel/Buildroot 定制及系统级 A/B OTA。RS485/Modbus RTU 暂不纳入本轮必做范围。仅把**有实际证据验证**的事项标记为完成。

## 当前进度（截至 D25-2 S7）

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
- **D25-2 进程生命周期管理：S1–S7 全部完成**。
  - 设计：[D25-2 方案](docs/D25-2-process-lifecycle-design.md)
  - S1 + 两轮安全修正（pidfile 身份校验）：**145 项**单元测试
  - S2 自管 Supervisor：44 项单测 + 真实 ARM Gateway 端到端
  - S3 升级流程接入：D23 回归 **27/27**
  - S3-R 复审修正：包校验前置（损坏包不再导致业务中断）
  - S3-R2 校验补全：前置校验扩展为 **activate 的全部只读条件**，
    两者**共用同一个 `activate_check()`**，避免规则分叉
  - S3-R3 测试补强：负向用例覆盖 **7 个校验分支**（前置 40 → 73 项）
  - S3-R4 激活前恢复：停止 Gateway 之后的失败路径统一走
    `abort_after_stop()`；修复「端口被外部占用时谎报 `restored and L1-ready`」
    的假阳性（改用 `/proc/net/tcp` inode + fd 反查确认端口属主）
  - S4 故障注入矩阵 **F1–F10，67 项 PASS**。修正三处真实缺陷：
    - 停止超时后不发 SIGKILL —— 该理由只对 D 状态成立，却被用来处理
      「用户态忽略 SIGTERM」，把可自动恢复的故障变成必须人工介入
    - 新增 `ota_oplog`（`last_operation.json`），让「已停旧版但未完成
      切换」这段空档有记录可查
    - 并发 OTA 的锁冲突提示从裸 errno 改为可读原因
  - S5 Service Supervisor：**通过转移所有权消除 L3/L6**
    - L3（前置校验依赖 pidfile）：板端实测「删掉 pidfile 后 supervisor
      依然正确报告状态并能停止进程」
    - L6（停止外部进程 TOCTOU，pidfd 需 Linux 5.1+）：supervisor 停的是
      自己的子进程，pid 不可能被复用
    - 强制 IPC、不做 pidfile 回退：supervisor 不可用即拒绝升级
  - S5-R1 init 集成（本轮）：
    - **G4b 实现**（原 L12）：`/etc/inittab` 注册 `respawn`，supervisor
      崩溃后由 init 自动拉起；板端 **21/21 PASS**
    - **G4a 修正**：`S99 start` 现在会启动 Gateway（此前只启动 supervisor，
      与 G4a 的定义矛盾）
    - 修复 supervisor 双实例竞态：`service_ipc_listen` 的
      「探测 + unlink + bind」加文件锁
    - supervisor 支持**领养**孤儿 Gateway（`SMARTGATEWAY_RESPAWN=1`），
      避免 init 无限重拉一个必然失败的命令
  - S6 板端升级与回滚：v0.2.0→v0.3.0 正常升级；v0.3.0→v0.4.0
    注入北向 `ECONNREFUSED` 后回滚，旧版业务实测恢复
  - S7 板端掉电恢复：真实断电命中 `STARTING_NEW` 窗口，
    重启后 `current` 不悬空、OTA 拒绝自动继续、`recover` 收敛、
    `ack-operation` 后可继续升级
  - 详见
    [S1-R+S2](docs/D25-2-S1R-S2-supervisor.md)、
    [S2-R](docs/D25-2-S2R-review-fixes.md)、
    [S2-R2](docs/D25-2-S2R2-review-fixes.md)、
    [S3](docs/D25-2-S3-supervisor-integration.md)、
    [S3-R](docs/D25-2-S3R-review-fixes.md)、
    [S3-R2](docs/D25-2-S3R2-preflight-completeness.md)、
    [S3-R3](docs/D25-2-S3R3-negative-tests-and-pid-boundaries.md)、
    [S3-R4](docs/D25-2-S3R4-pre-activation-recovery.md)、
    [S4](docs/D25-2-S4-fault-injection-matrix.md)、
    [S5](docs/D25-2-S5-service-supervisor.md)、
    [S6+S7](docs/D25-2-S6-S7-board-upgrade-and-recovery.md)
- **注意：本项目无 CI**，所有 PASS 均为本地/板端手工执行结果

## 下一阶段顺序

1. **D25-2 已全部完成**（S1–S7，含 G4a/G4b）。板端验收结论：
   - 开机自动启动 Gateway（G4a）
   - supervisor 崩溃后由 init 自动拉起（G4b，`inittab respawn`）
   - 真实 OTA 升级、北向故障回滚、真实断电恢复均已实测

2. **系统定制与 A/B OTA 扩展**：新分支 `feature/imx6ull-system-ab-ota` 已建立。先完成平台/存储基线与恢复能力核验，再复现并裁剪 Kernel、定制 Buildroot RootFS，最后设计 U-Boot A/B 启动与系统 OTA。
3. 新扩展文档按工程组成部分组织，入口见 [System A/B 文档索引](docs/system-ab/README.md)。其中 eMMC p1/p3 用途、U-Boot 环境存储位置和原厂恢复能力尚未确认；未通过恢复门槛前不修改分区表或写入 eMMC。

原 D16 后路线图保留为历史计划记录；本轮系统扩展以 `docs/system-ab/` 下的模块化文档为准。

## 已知限制（不报告为 PASS）

| 编号 | 内容 | 原因 |
| --- | --- | --- |
| L10 | 「SIGKILL 也无效」分支无集成测试 | D 状态无法构造：x86 与板端均缺 FUSE 开发包、无 NFS、无 root |
| L11 | 领养的 Gateway 管理能力弱于原生 | respawn 模式下新 supervisor 不是其父进程，`waitpid` 无效，只能 kill + 轮询 |
| L13 | 领养路径的停止操作是弱保证 | 发信号前已复核 `(pid, starttime)`，但 `read_stat` 与 `kill` 之间仍有窗口 —— 需要 pidfd（Linux 5.1+），板端 4.9.88 不可用 |
| — | 串口模式未板端验证 | 全部验收以 `--no-serial` 运行，`/dev/ttymxc2` 路径未实测 |
| — | 真实北向云服务器未验证 | 上游为本地 Mock |
| — | eMMC 坏块 / 只读挂载未验证 | 未做存储层故障注入 |
| — | 无 CI | 所有 PASS 均为手工执行 |

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

### 系统级扩展基线（本轮板端实测）

| 项目 | 结果 |
|---|---|
| Linux | 4.9.88，ARMv7，SMP PREEMPT |
| Kernel 编译器 | Linaro GCC 6.2.1 |
| eMMC user area | 7,634,944 × 512-byte sectors，约 3.64 GiB |
| p1 | 1,000 MiB，用途待确认 |
| p2 | 1,500 MiB，当前 ext4 根文件系统；677 MiB used / 706 MiB avail |
| p3 | 500 MiB，用途待确认 |
| p3 后尾部 | 按扇区算术推算约 726.97 MiB，分区表/原厂占用待核验 |
| MemTotal | 500,948 KiB |
| CMA | 320 MiB total，约 220.9 MiB free（采样时点） |

详见 [板端平台与 eMMC 基线](docs/system-ab/01-platform-baseline.md) 和 [U-Boot 启动链基线](docs/system-ab/02-boot-chain-and-uboot.md)。

### 硬件与软件基线（历史记录）

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

- 原有 BSP 驱动与应用 OTA 基线保留在 `feature/bsp-imx6ull`；系统定制与 A/B OTA 在 `feature/imx6ull-system-ab-ota` 开发，不在 `main` 直接开发。
- 按 `docs(bsp)`、`build(bsp)`、`feat(driver)`、`test(bsp)` 等 Conventional Commits 形式提交，每个提交聚焦一个主题。
- 每阶段保留命令、实测输出、验收结果与已知问题；计划中的工作不得写成 PASS。
- 原厂恢复介质与可靠恢复方式未确认前，不执行 `dd` 写块设备、`mmc write`、`saveenv`、刷写 U-Boot 或覆盖现有 DTB / Kernel。
- 不把原厂镜像、工具链或私人设备资料上传到公开仓库。
