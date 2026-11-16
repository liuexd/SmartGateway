# G1：恢复基线与主机工程准备验收报告

**状态：** HOST-VERIFIED（主机侧检查与 `/dev/root` 防误写回归已通过；板端真机外部恢复与全量备份仍为 **PENDING BOARD**）  
**分支：** `feature/imx6ull-system-ab-ota`  
**设计文档：** [07-g1-recovery-baseline-and-toolchain.md](../../system-ab/07-g1-recovery-baseline-and-toolchain.md)

## 1. 本阶段验证范围与边界

- **主机已完成并实测：** 分支与源码基线核查、工具链检查、既有应用/Supervisor 回归、`/dev/root` 与 `/dev/mmcblk1*` 只读保护与防自毁恢复备份脚本测试。
- **未覆盖（显式记为 PENDING BOARD）：** 外部 SD 恢复介质启动成功实录、板端全量离线备份（`--mode full` + `SHA256SUMS`）、回刷恢复路径演练（受当前不写板边界约束）。

## 2. 主机核查命令与实测结果

| 优先级 | 检查项 | 命令 | 实测结果 | 状态 |
|---|---|---|---|---|
| P0 | 主机仓库与工具链核查 | `bash bsp/tools/g1_check_host_env.sh` | `PASS=12 FAIL=0` | **PASS (HOST)** |
| P0 | 备份脚本 `/dev/root` 与 eMMC 防误写测试 | `sh -n bsp/tools/g1_emmc_backup_verify.sh && python3 -m unittest -v tests/test_g1_backup_guard.py` | `Ran 4 tests ... OK`（覆盖 `/dev/root` 指向 `mmcblk1p2` 拒绝、未证 `PARTUUID` fail-closed 拒绝、直指 `mmcblk1p1/p2/p3` 拒绝、`rw` 下拒绝抓拍、外部 SD 卡启动全量备份与 `SHA256SUMS` 校验通过） | **PASS (HOST)** |
| 基线 | 现有主机 C 单元测试 | `make test` | 10/10 PASS（`test_crc16`..`test_tcp_send_timeout`） | **PASS (HOST)** |
| 基线 | Service Supervisor 回归 | `make -C bsp/apps/service_supervisor test` | `service_ipc_test` 58/58 PASS；`service_supervisor_test` 35/35 PASS | **PASS (HOST)** |
| **P0** | **外部 SD/恢复介质启动、板端离线全量备份 SHA256 与恢复路径演练** | 板端真机执行 | **尚未在目标开发板执行** | **PENDING (BOARD)** |

## 3. 安全结论

- 当前开发板 `/dev/mmcblk1` 分区表、U-Boot 本体、环境变量及 `p2` 系统保持**零改动**。
- 必须先完成上表最后一行 `P0 PENDING (BOARD)` 实操取证，方可进入任何写板环节。
