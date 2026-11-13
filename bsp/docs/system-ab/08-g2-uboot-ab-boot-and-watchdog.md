# G2：U-Boot A/B 启动管理、持久化计数与 Watchdog 回滚设计

**状态：** HOST-TESTED（主机侧交叉编译、真实 U-Boot 2017.03 `fw_printenv`/`fw_setenv` 二进制环境镜像仿真及 C/Python 单元测试全部通过；尚未刷写开发板）

## 1. U-Boot 2017.03 真实持久化 `bootcount` 后端调研结论

直接审计主机 `~/Desktop/Uboot-2017.03` 源码树与编译产物确认：

| 候选后端 | U-Boot 2017.03 支持情况 | 板端硬件前提 | 结论 |
|---|---|---|---|
| `drivers/bootcount/Kconfig` | **不存在**该 Kconfig 文件 | — | 必须在板级头文件 `include/configs/mx6ullevk.h` 中配置宏 |
| `CONFIG_BOOTCOUNT_RAM` (`bootcount_ram.c`) | 存在 | 依赖 DRAM 掉电保持 | **不满足**：真实冷断电后 DRAM 丢失 |
| `CONFIG_BOOTCOUNT_I2C` (`bootcount_i2c.c`) | 存在 | 需外部 I2C RTC/EEPROM | **不满足**：板级 DTS 仅挂载 `ap3216@1e` 与 `wm8960@1a` |
| SoC SNVS RTC (`SNVS_LPGPR`) | 驱动可配，但板端无持久供电 | G0-1B 实测板端 RTC 重置为 `1970-01-06` | **不可靠**：无备份电池，断电丢失 |
| **`CONFIG_BOOTCOUNT_ENV` + `CONFIG_ENV_OFFSET_REDUND`** | **原生完整支持**（`drivers/bootcount/bootcount_env.c` + `common/env_mmc.c` + `tools/env/fw_env.c`） | 仅需 eMMC `pre-p1` 预留空间 | **采用方案**：冷断电完全持久化，双拷贝 CRC32 抗写中断损坏 |

关键机制核实：
- `bootcount_env.c` 仅在 `upgrade_available != 0` 时写回 `bootcount` 并调用 `saveenv()`，**正常稳态启动对 eMMC 零额外磨损**。
- 冗余环境布局：`0xE0000` (8 KiB) + `0xE2000` (8 KiB)，结束地址 `0xE4000`，严格位于 `p1` 起始边界（sector `4098` = `0x200400`）之前。
- 冗余环境头部格式由单份 4 字节 CRC32 变为 5 字节（4B LE CRC32 + 1B 递增 `flags`）。因此后续板端迁移（G4/G5 阶段经 G1 放行后）需按新工具与双区镜像迁移，禁止将单区工具写向冗余区。

## 2. 环境变量状态机与 SWUpdate 变量隔离

为彻底防止与 Buildroot 中 SWUpdate 2019.11 的环境变量冲突：

- `swupdate.config` 启用 `CONFIG_SURICATTA_STATE_BOOTLOADER="update"`；`core/swupdate.c` 在升级事务中使用 `recovery_status`（`in_progress` / `failed` / unset）。
- 原厂 `mx6ullevk.h` 中 `"updateset"` 会在 `update=undefined` 时自动执行 `saveenv`。本设计改为在默认环境保留 `"update=yes"` 并移除 `updateset` 自动写盘，**A/B 控制器严格禁止写入 `update` 或 `recovery_status`**。

### 变量定义

| 变量名 | 默认值 | 取值/说明 | 所有权 |
|---|---|---|---|
| `sgw_active_slot` | `2` | `1` / `2`，默认优先保留当前 `p2` | U-Boot & `sgw_bootctl` |
| `sgw_last_good_slot` | `2` | `1` / `2`，最近一次健康确认的稳定槽位 | U-Boot `altbootcmd` / `sgw_bootctl confirm` |
| `sgw_pending_slot` | `none` | `none` / `1` / `2`（使用 `none` 而非空串，保证 U-Boot hush `test` 安全） | `sgw_bootctl stage` / U-Boot `altbootcmd` |
| `sgw_ab_state` | `confirmed` | `confirmed` / `trial` / `rolled_back` | 状态机共用 |
| `bootlimit` | `3` | 允许试启动最大连续失败次数 | U-Boot `autoboot.c` |
| `bootcount` | `0` | 当前试启动已尝试次数 | U-Boot `bootcount_env.c` |
| `upgrade_available` | `0` | `0`=稳态；`1`=试启动激活（使能 `bootcount` 持久化增量） | `sgw_bootctl` / U-Boot `altbootcmd` |
| `sgw_rollback_count` | `0` | 累计自动/手动回滚次数 | U-Boot `altbootcmd` (`setexpr`) / `sgw_bootctl` |
| `sgw_last_error` | `none` | 最近错误码，如 `bootlimit_exceeded`、`health_timeout` | U-Boot / `sgw_bootctl` |
| `update`, `recovery_status` | `yes` / (SWUpdate管理) | SWUpdate 保留变量，A/B 仅作只读预检拦截 | SWUpdate 独占 |

### 状态转移图

```text
  [confirmed: active=2, last_good=2, upgrade_available=0]
           │
           │ sgw_bootctl stage --target 1 --running 2
           ▼
  [trial: active=1, pending=1, last_good=2, upgrade_available=1, bootcount=0]
           │
           ├─► U-Boot 启动 1..3 次 (bootcount = 1..3 <= bootlimit=3)
           │      ├─► 健康确认成功: sgw_bootctl confirm --running 1 --health-ok 1
           │      │      └─► [confirmed: active=1, last_good=1, pending=none, upgrade_available=0, bootcount=0]
           │      └─► 崩溃 / 内核 panic=5 / WDOG1 60s 超时重启
           │             └─► 下一次重启进入 U-Boot，bootcount 递增
           │
           └─► 第 4 次重启 (bootcount=4 > bootlimit=3)
                  └─► U-Boot 执行 altbootcmd:
                         active=last_good(2), pending=none, state=rolled_back,
                         last_error=bootlimit_exceeded, rollback_count+=1,
                         upgrade_available=0, bootcount=0, saveenv -> 启动 p2
```

## 3. 硬件 Watchdog 与 `altbootcmd` 超时恢复路径

直接核实 U-Boot `drivers/watchdog/imx_watchdog.c` 与 Linux 4.9.88 `drivers/watchdog/{imx2_wdt.c,watchdog_dev.c}`：

1. **U-Boot 阶段**：启用 `CONFIG_HW_WATCHDOG`、`CONFIG_IMX_WATCHDOG` 及 `CONFIG_WATCHDOG_TIMEOUT_MSECS=60000`（`WDOG1` 硬件倒计时 60 秒；写 `WCR.WDE` 后在下次 POR/复位前不可由软件关闭）。U-Boot 串口命令行及加载路径内置 `WATCHDOG_RESET()` 喂狗，不会妨碍串口调试。
2. **Linux 内核与挂载失败恢复**：`mmcargs` 增加 `panic=5`。若新槽位内核 panic、缺少 ext4 rootfs 或缺少 `/sbin/init`，内核 5 秒后强制复位；若发生早期死锁无反应，`WDOG1` 在 60 秒内强制硬件复位。
3. **关键内核细节与用户态看门狗交接**：审计 Linux 4.9.88 `watchdog_dev.c` 可知，当 `WDOG1` 已在 U-Boot 启动且用户态尚未打开 `/dev/watchdog` 时，`watchdog_need_worker()` 返回真，内核工作队列会在后台持续自动喂狗；一旦用户态打开 `/dev/watchdog`，`WDOG_ACTIVE` 置位，内核停止自动喂狗并由用户态守护进程接管心跳。
   - **试启动健康超时策略**：试启动阶段早期用户态守护打开 `/dev/watchdog`，仅在核心服务健康检查通过并执行 `sgw_bootctl confirm` 期间正常喂狗；若超过试启动健康预算（`ab_watchdog_trial_expired()` 判定过期）仍未确认成功，守护停止喂狗（或主动停止不发送 magic close），触发 `WDOG1` 硬件复位，使 `bootcount` 累加并经 3 次失败后由 `altbootcmd` 自动回滚至旧槽位。

## 4. 代码与构建产物

| 文件 | 角色 |
|---|---|
| [bsp/uboot/patches/0001-mx6ullevk-ab-bootcount-redund-env-watchdog.patch](../../uboot/patches/0001-mx6ullevk-ab-bootcount-redund-env-watchdog.patch) | U-Boot 2017.03 板级 A/B、`BOOTCOUNT_ENV`、冗余 MMC env 与 `IMX_WATCHDOG` 补丁 |
| [bsp/uboot/build_uboot_ab.sh](../../uboot/build_uboot_ab.sh) | 隔离构建脚本，输出 `build/uboot-ab/arm/{u-boot-dtb.imx,fw_printenv,u-boot.cfg,System.map}` 与 `host/{fw_printenv,fw_setenv}` |
| [bsp/uboot/fw_env.ab.config](../../uboot/fw_env.ab.config) | 冗余 MMC env 配置（`/dev/mmcblk1 0xe0000 0x2000` + `0xe2000 0x2000`） |
| [bsp/apps/boot_ab/ab_state.h](../../apps/boot_ab/ab_state.h), [ab_state.c](../../apps/boot_ab/ab_state.c) | A/B 状态机校验、预检查、试启动、确认、回滚与 Watchdog 超时判断核心逻辑 |
| [bsp/apps/boot_ab/fw_env_backend.h](../../apps/boot_ab/fw_env_backend.h), [fw_env_backend.c](../../apps/boot_ab/fw_env_backend.c) | 无 shell 注入的 `fw_printenv`/`fw_setenv -s -` 封装，含 SWUpdate 变量写保护白名单 |
| [bsp/apps/boot_ab/sgw_bootctl.c](../../apps/boot_ab/sgw_bootctl.c) | ARM/主机命令行工具：`status`、`validate`、`stage`、`confirm`、`rollback`、`watchdog-check` |
| [tests/test_g2_uboot_ab.py](../../../tests/test_g2_uboot_ab.py) | 基于真实 U-Boot 2017.03 编译产物与 4 MiB 仿真 eMMC 镜像的端到端主机测试 |
