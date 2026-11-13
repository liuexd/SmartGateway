# G2：U-Boot A/B 启动管理与主机端仿真验收报告

**状态：** HOST-TESTED（主机交叉编译、真实 U-Boot 2017.03 `fw_printenv`/`fw_setenv` 冗余环境校验及仿真回滚全部通过；板端上电/断电实测尚未执行）  
**分支：** `feature/imx6ull-system-ab-ota`  
**设计文档：** [08-g2-uboot-ab-boot-and-watchdog.md](../../system-ab/08-g2-uboot-ab-boot-and-watchdog.md)

## 1. 验收边界声明

本报告中标记 `PASS (HOST)` 的项目均为开发虚拟机上的真源码编译、二进制符号核查、C 单元测试及基于真实 `U-Boot 2017.03` 编译产物的 4 MiB eMMC 仿真镜像集成测试。**尚未刷写开发板，任何板端真机行为不得视为已验收完成。**

## 2. 真实 U-Boot 2017.03 编译与符号核查

执行隔离构建 `bash bsp/uboot/build_uboot_ab.sh`（不污染 `~/Desktop/Uboot-2017.03`）：

| 验证项 | 命令 / 依据 | 实测结果 | 状态 |
|---|---|---|---|
| 隔离打补丁并构建 `u-boot-dtb.imx` | `bsp/uboot/build_uboot_ab.sh` | 退出码 `0`；生成 `build/uboot-ab/arm/u-boot-dtb.imx` (515K) 与默认 env | **PASS (HOST)** |
| 编译配置宏核查 (`u-boot.cfg`) | `grep` 验证 `build/uboot-ab/arm/u-boot.cfg` | 确认 `CONFIG_BOOTCOUNT_LIMIT`, `CONFIG_BOOTCOUNT_ENV`, `CONFIG_ENV_OFFSET_REDUND`, `CONFIG_SYS_REDUNDAND_ENVIRONMENT`, `CONFIG_HW_WATCHDOG`, `CONFIG_IMX_WATCHDOG`, `CONFIG_WATCHDOG_TIMEOUT_MSECS 60000`, `CONFIG_CMD_EXT4 1`, `CONFIG_CMD_SETEXPR 1` | **PASS (HOST)** |
| 链接符号核查 (`System.map`) | `grep` 验证 `build/uboot-ab/arm/System.map` | 确认 `env_relocate_spec`, `bootcount_store`, `bootcount_load`, `hw_watchdog_reset`, `hw_watchdog_init` 已链接 | **PASS (HOST)** |
| 默认环境 p2 优先与变量隔离 | 真实 `build/uboot-ab/host/fw_printenv` 对未初始化镜像 | 输出 `sgw_active_slot=2`, `sgw_last_good_slot=2`, `sgw_pending_slot=none`, `sgw_ab_state=confirmed`, `bootlimit=3`, `bootcount=0`, `upgrade_available=0`, `update=yes` 及 `ab_select_slot`/`ab_boot_fallback`/`altbootcmd` | **PASS (HOST)** |
| ARM `fw_printenv` 与 `sgw_bootctl` 交叉编译 | `build_uboot_ab.sh` & `make -C bsp/apps/boot_ab arm-check` | 均为 `ELF 32-bit LSB executable, ARM, EABI5`；`sgw_bootctl` 42K 仅依赖 `libc.so.6` (`GLIBC_2.4`) | **PASS (HOST)** |

## 3. 主机单元测试与仿真集成测试结果

| 测试套件 | 命令 | 测试覆盖内容 | 实测结果 | 状态 |
|---|---|---|---|---|
| 状态机 C 单元测试 | `make -C bsp/apps/boot_ab test` (`ab_state_test`) | 默认保留 p2、槽位与状态解析、SWUpdate 保留变量判断、禁止写运行槽与并发升级拦截、`bootcount=1..3` 与第 4 次 `altbootcmd` 自动回滚、双向 `p1<->p2` 确认、Watchdog 超时判断 | `PASS=100 FAIL=0` | **PASS (HOST)** |
| `fw_env` 后端 C 单元测试 | `make -C bsp/apps/boot_ab test` (`fw_env_backend_test`) | 脚本白名单拦截 `update` / `recovery_status` / 非法变量写入、子进程无 shell 读写往返与 SWUpdate 变量保持不变 | `PASS=25 FAIL=0` | **PASS (HOST)** |
| 真实 `fw_env` + 4 MiB 仿真 eMMC Python 集成测试 | `python3 -m unittest -v tests/test_g2_uboot_ab.py` | (1) 源码后端与宏/符号校验；(2) 双拷贝 CRC32 (`0xE0000`+`0xE2000`) 与递增 `flags`；(3) `stage 1` -> 3 次试启动失败 -> 第 4 次触发 `altbootcmd` 回滚 `p2` 且 `update`/`recovery_status` 保持原值；(4) `p2->p1->p2` 健康确认双向切换；(5) 同槽位与 `recovery_status=in_progress` 拦截；(6) 抹除新副本前 128B 后从备用副本无缝恢复；(7) `watchdog-check` CLI 超时判断 | `Ran 7 tests ... OK` | **PASS (HOST)** |

## 4. 待板端验证清单（下一阶段前置条件）

以下命令/场景尚未执行，只能在完成 G1 板端离线备份与恢复演练后实施：

1. 开发板串口观察新版 U-Boot `WDOG1` 60 秒超时硬复位及 `printenv` 验证。
2. 真机冷断电前后 `bootcount` 跨断电持久化递增与 `bootlimit=3` 自动回滚到 `p2`。
3. Linux `/dev/watchdog` 由用户态试启动健康守护进程接管后的超时复位闭环。
