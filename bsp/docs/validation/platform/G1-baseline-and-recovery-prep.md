# G1：恢复基线与主机工程准备验收报告

**状态：** HOST-VERIFIED（板端只读基线采集已于 G0 完成；板端离线备份与 SD 卡恢复演练待现场执行）  
**分支：** `feature/imx6ull-system-ab-ota`  
**设计文档：** [07-g1-recovery-baseline-and-toolchain.md](../../system-ab/07-g1-recovery-baseline-and-toolchain.md)

## 1. 本阶段验证范围与边界

- **覆盖范围：** 开发分支核实与同步、上游 U-Boot/Linux/Buildroot 源码基线与工具链验证、原有应用/Supervisor 基线回归、主机基线检查脚本与只读 eMMC 备份脚本编写。
- **未覆盖（未标记完成）：** 开发板真机外部 SD 恢复盘启动演练、真机全量 eMMC 离线备份执行（受“禁止写板及修改当前 p2 系统”约束，留待现场只读介质准备后执行）。

## 2. 主机核查命令与实测结果

| 检查项 | 命令 | 实测结果 | 状态 |
|---|---|---|---|
| 主机仓库与工具链核查 | `bash bsp/tools/g1_check_host_env.sh` | `PASS=12 FAIL=0`；确认 `SmartGateway-BSP` (`feature/imx6ull-system-ab-ota`), `SmartGateway` (`main`), `Uboot-2017.03` (`8ba4c5bb19`, `dirty=0`), `Linux-4.9.88` (`d0b34fd9c024`), `Buildroot_2020.02.x` (`37ba456`), Linaro GCC `6.2.1`, Buildroot GCC `7.5.0` | **PASS (HOST)** |
| 备份脚本语法检查 | `sh -n bsp/tools/g1_emmc_backup_verify.sh` | 退出码 `0`（仅对 `/dev/mmcblk1` 只读，拒绝写入 eMMC 目标，检测 `rw` 挂载的 `p2`） | **PASS (HOST)** |
| 现有主机 C 单元测试基线 | `make test` | 10 个测试二进制（`test_crc16`..`test_tcp_send_timeout`）全部通过 | **PASS (HOST)** |
| Service Supervisor 基线回归 | `make -C bsp/apps/service_supervisor test` | `service_ipc_test` 58/58 PASS；`service_supervisor_test` 35/35 PASS | **PASS (HOST)** |
| 外部 SD 卡 / 板端真机恢复演练 | `g1_emmc_backup_verify.sh --mode full` (板端) | 尚未在目标板执行 | **PENDING (BOARD)** |

## 3. 安全结论

- 当前开发板 `/dev/mmcblk1` 分区表、U-Boot 本体、U-Boot 环境变量及 `p2` 系统均**未做任何改动**。
- 在板端真机完成 `g1_emmc_backup_verify.sh` 离线备份及恢复路径确认前，仍不放行向 eMMC 刷写任何 A/B 镜像或新 U-Boot。
