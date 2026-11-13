# G1：恢复基线、工具链核查与分支建立

**状态：** HOST-VERIFIED（主机侧工程基线与只读脚本就绪；板端备份与恢复演练待执行）  
**分支：** `feature/imx6ull-system-ab-ota`（基于 `de0e4a3`）

## 1. 阶段目标与安全边界

G1 建立可追溯、可恢复的开发基线，不再以原厂 `.swu` 或 `sw-description` 作为后续开发前置条件，同时严格保护当前板端可正常启动的 `/dev/mmcblk1p2`（`p2`）：

- 不修改开发板分区表、U-Boot 本体、U-Boot 环境变量及 eMMC 数据。
- 保持上游 BSP 源码树（`Uboot-2017.03`、`Linux-4.9.88`、`Buildroot_2020.02.x`）干净或独立受控；本项目定制以补丁与隔离构建脚本保存在 `SmartGateway-BSP`。
- 在任何真实写板前，完成外部恢复介质准备、离线备份及 SHA-256 校验。

## 2. 仓库与上游源码基线

通过主机检查脚本 [bsp/tools/g1_check_host_env.sh](../../tools/g1_check_host_env.sh) 核实：

| 组件 | 路径（开发虚拟机） | 分支 / Commit | 工作区状态 | 说明 |
|---|---|---|---|---|
| BSP 主仓库 | `~/Desktop/project/SmartGateway-BSP` | `feature/imx6ull-system-ab-ota` (`de0e4a3`) | 独立开发分支 | 系统 A/B 与 G1–G6 主分支 |
| 应用参考仓库 | `~/Desktop/project/SmartGateway` | `main` (`f2dfe9c`) | 只读基线 | 应用层与服务端逻辑参考 |
| U-Boot 源码 | `~/Desktop/Uboot-2017.03` | `8ba4c5bb19` | `dirty=0` 干净 | 与板端基线 `U-Boot 2017.03-g8ba4c5bb19-dirty` 同源 |
| Linux 内核源码 | `~/Desktop/Linux-4.9.88` | `d0b34fd9c024` | 1 处本地 DTS 修改 | `arch/arm/boot/dts/100ask_imx6ull-14x14.dts` |
| Buildroot 源码 | `~/Desktop/Buildroot_2020.02.x` | `37ba456` | 基线 | 含 `100ask/nxp-imx6ull` 配置与 SWUpdate 2019.11 |

## 3. 交叉编译工具链矩阵

| 工具链 | 路径 | 版本 | 用途 |
|---|---|---|---|
| Linaro GCC 6.2.1 | `~/Desktop/ToolChain/gcc-linaro-6.2.1-2016.11-x86_64_arm-linux-gnueabihf/bin/arm-linux-gnueabihf-gcc` | `6.2.1 20161016` | 编译 U-Boot 2017.03 与 Linux 4.9.88 内核（U-Boot 2017.03 不兼容过高版本 GCC 内核/引导宏约束） |
| Buildroot SDK GCC 7.5.0 | `~/Desktop/ToolChain/arm-buildroot-linux-gnueabihf_sdk-buildroot/bin/arm-buildroot-linux-gnueabihf-gcc` | `7.5.0` (glibc 2.30 sysroot) | 交叉编译用户空间程序（`sgw_bootctl`、目标端 `fw_printenv`/`fw_setenv`、`ota_agent`、`service_supervisor`） |

## 4. 已确认板端分区布局（只读基线）

依据 G0 已采集板端数据，eMMC user area 共 `7,634,944` 个 512-byte sectors（约 `3.64 GiB`）：

| 设备 | 起始 Sector | 扇区数 | 逻辑容量 | 当前角色 |
|---|---:|---:|---:|---|
| 预留区 (`pre-p1`) | `0` | `4,098` | `2,098,176 B` (~2.0 MiB) | MBR、U-Boot `.imx` 镜像及 U-Boot 环境（`0xE0000` / `0xE2000`） |
| `/dev/mmcblk1p1` | `4,098` | `2,048,000` | `1000 MiB` | A/B 目标槽位 `rootfs-1`（待 G4/G5 正式格式化写入；当前禁止提前写板） |
| `/dev/mmcblk1p2` | `2,052,098` | `3,072,000` | `1500 MiB` | **当前可启动 active ext4 rootfs（`rootfs-2`，优先保留）** |
| `/dev/mmcblk1p3` | `5,124,098` | `1,024,000` | `500 MiB` | MBR type `0x0C`，待作为共享持久数据候选区（写板前须完成备份） |

> 详情与历史只读采集见 [01-platform-baseline.md](01-platform-baseline.md) 与 [G0-1B-p1-p3-rtc-followup.md](../validation/platform/G0-1B-p1-p3-rtc-followup.md)。

## 5. 备份与恢复路径设计

新增板端只读采集与校验脚本 [bsp/tools/g1_emmc_backup_verify.sh](../../tools/g1_emmc_backup_verify.sh)：

1. **只读保护**：对 `/dev/mmcblk1` 仅执行只读读取，拒绝将备份目标目录置于 `/dev/mmcblk1*`。
2. **`metadata` 模式**（默认）：导出 `sfdisk -d`、`fdisk -l`、`fw_printenv`、`/proc/partitions`、`/proc/mounts` 以及 `pre-p1` 前 `4,098` sectors（`emmc-reserved-pre-p1.bin`），生成并校验 `SHA256SUMS`。
3. **`full` 模式**：备份 `p1`/`p2`/`p3` 及 `boot0`/`boot1` 并执行 `sha256sum -c SHA256SUMS`。脚本默认检测 `/` 若以 `rw` 挂载在 `mmcblk1p2` 则拒绝生成 `full` 镜像（防止不一致快照），要求从外部 SD 恢复介质启动或以 `ro` 只读重挂后采集（显式传入 `--allow-rw-snapshot` 仅用于非一致性诊断）。
4. **恢复路径顺序**：
   - 第一优先：外部 SD 卡启动盘 / MFGTool / USB 下载模式恢复（不依赖 eMMC `p2` 与原厂 `.swu`）。
   - 第二优先：在确认不覆盖 `p2` 前，任何环境或槽位实验必须先保存原始 `emmc-reserved-pre-p1.bin` 与 `uboot-env.txt`。

## 6. 当前阶段交付物清单

- [bsp/tools/g1_check_host_env.sh](../../tools/g1_check_host_env.sh) — 主机仓库/工具链核查脚本（HOST-VERIFIED：`PASS=12 FAIL=0`）
- [bsp/tools/g1_emmc_backup_verify.sh](../../tools/g1_emmc_backup_verify.sh) — 板端只读备份与 SHA-256 校验脚本（HOST-VERIFIED 语法与逻辑；待板端执行）
- [G1-baseline-and-recovery-prep.md](../validation/platform/G1-baseline-and-recovery-prep.md) — G1 阶段验收报告
