# G2：U-Boot A/B 启动管理与主机端仿真验收报告

> **2026-11-16 安全整改说明：** 以下为历史记录，其中的通过结论和操作说明以[最新主机安全整改记录](G1-G2-host-safety-remediation.md)为准。G1/G2 均为 BLOCKED；备份脚本接口已变化，G2 可信健康服务尚未实现，当前不得部署或写板。

**状态：** HOST-TESTED（已完成 fail-closed 健康确认、`/dev/watchdog` 用户态守护与单份->冗余 env 迁移的主机测试；板端真机 U-Boot 启动、`p1` 实机启动与 `WDOG1` 硬件复位尚未执行）  
**分支：** `feature/imx6ull-system-ab-ota`  
**设计文档：** [08-g2-uboot-ab-boot-and-watchdog.md](../../system-ab/08-g2-uboot-ab-boot-and-watchdog.md)

> **编号衔接说明：** 原 `readiness-and-roadmap/02-g1-g6-implementation-plan.md` 中旧 G2（Linux 4.9.88 / Buildroot 2020.02 裁剪前基线构建复现）已统一纳入 **G3 前置门禁（G3-0）**，避免进入 G3 裁剪前遗漏基线镜像构建与测量基准。

## 1. P0/P1 整改与证据边界矩阵

| 优先级 | 验证 / 证据项 | 当前状态 | 证据与边界说明 |
|---|---|---|---|
| **P0** | **`sgw_bootctl confirm` 默认拒绝及可信健康/槽位验证** | **PASS (HOST)** | 已移除 `health_ok=1` 默认值，改用 `ab_verify_system_health()`（校验 `/proc/cmdline` + `/proc/mounts`、系统关键文件与真实 `service_supervisor` UNIX socket `STATUS` 存活 PID）；`ab_health_test` (`26/26 PASS`) 与 `test_fail_closed_confirm_and_trusted_health_checks` 验证全覆盖 |
| **P0** | **`sgw_watchdog_guard` 用户态接管、确认及超时不写 `'V'` 测试** | **PASS (HOST)** | 通过实时 FIFO 模拟 `/dev/watchdog` 与动态 `supervisor.sock` 验证：`TRIAL_CONFIRMED` 后写 `'V'`；超预算仍未健康则记 `health_timeout`、停喂、不写 `'V'`、退出码 `3`。硬件 `WDOG1` 真实芯片复位须板端复测 |
| **P1** | **单份 U-Boot 环境迁移至冗余环境安全测试** | **PASS (HOST)** | 实际编译未打补丁的单区 `host-single/fw_printenv` 生成 4B CRC 环境，校验冗余工具会报 `Bad CRC`，再经 `migrate_single_to_redund_env.sh` 保留 `ethaddr` 与 `update=yes` 并生成双份有效 5B CRC32+flags |
| **P0** | 真实 U-Boot 2017.03 编译产物、宏与脚本核查 | **PASS (HOST)** | `build_uboot_ab.sh` 构建 `u-boot-dtb.imx` (515K)、主机/ARM `fw_printenv` 与 `sgw_bootctl` / `sgw_watchdog_guard`；直接核验 `fw_printenv` 输出的 `bootcmd`、`ab_boot_fallback`、`altbootcmd`、`mmcargs`；但**受限于主机无真实 i.MX6ULL/QEMU 目标机，U-Boot 启动决策仍由 Python 结合真实 `fw_printenv`/`fw_setenv` 二进制模拟** |
| **P1** | **U-Boot 在目标板真正执行 A/B `bootcmd` / `altbootcmd` 记录** | **NOT RUN (BOARD)** | 须在 G1 板端恢复门禁通过后由串口日志补齐 |
| **P1** | **`p1` 含有效 Kernel、DTB、RootFS 的真机启动验证** | **NOT RUN (BOARD)** | 须待 G3 基线构建与 G4 写入 `p1` 后实机串口验证 |
| **P1** | **板端真机 Watchdog 硬件复位、冷断电计数、超限回滚串口日志** | **NOT RUN (BOARD)** | 主机仅完成守护进程与冗余二进制计数验证，真机 POR/WDOG1 复位日志待上板补齐 |

## 2. 本轮最新实测命令与结果

| 套件 | 命令 | 结果 |
|---|---|---|
| U-Boot 单/双环境工具与镜像构建 | `bash bsp/uboot/build_uboot_ab.sh` | 退出码 `0`；产出 `u-boot-dtb.imx` (515K)、`host/fw_printenv`、`host-single/fw_printenv`、`arm/fw_printenv`、`u-boot.cfg`、`System.map` |
| `boot_ab` C 单元测试 & ARM 检查 | `make -C bsp/apps/boot_ab clean test arm-check` | `ab_state_test: PASS=100 FAIL=0`；`fw_env_backend_test: PASS=25 FAIL=0`；`ab_health_test: PASS=26 FAIL=0`；ARM `sgw_bootctl` (76K, ELF32 ARM, `GLIBC_2.28`) 与 `sgw_watchdog_guard` (90K, ELF32 ARM, `GLIBC_2.28`) |
| G1 防护与 G2 全链路 Python 集成测试 | `python3 -m unittest -v tests/test_g1_backup_guard.py tests/test_g2_uboot_ab.py` | `Ran 10 tests in 1.577s OK` |
