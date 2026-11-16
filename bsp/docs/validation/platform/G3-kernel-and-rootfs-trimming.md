# G3 验证报告：Linux 4.9.88 内核与 Buildroot 2020.02 RootFS 裁剪

- **状态：** `HOST-TESTED`（主机隔离交叉构建、镜像内容检查、ELF 动态库完整度检查及回归单元测试通过；**未触碰开发板 eMMC，`NOT RUN / PENDING BOARD` 项待恢复基线就绪后上板验证**）
- **基线提交：** `feature/imx6ull-system-ab-ota`
- **对应设计文档：** [09-g3-kernel-and-rootfs-trimming.md](../../system-ab/09-g3-kernel-and-rootfs-trimming.md) / [g3_package_audit.md](../../../rootfs/g3_package_audit.md)

## 1. G3-0 与 G3 内核实测对比（`build/kernel-g3/g3_kernel_metrics.json`）

| 指标 | G3-0 基线（`100ask_imx6ull_defconfig`） | G3 裁剪版（`g3_trimmed.fragment` + CMA 补丁） | 变化 |
|---|---:|---:|---:|
| `zImage` 体积 | `8,873,360 B` (`8.46 MiB`) | `4,848,520 B` (`4.62 MiB`) | **-45.4%** |
| `vmlinux` ELF 文件大小 | `23,665,688 B` | `12,267,588 B` | **-48.2%** |
| `vmlinux` `text+data+bss` (`size`) | `18,539,238 B` (`text=12,835,954`, `data=5,214,108`, `bss=489,176`) | `9,365,578 B` (`text=6,769,702`, `data=2,213,636`, `bss=382,240`) | **-49.5%** |
| `100ask_imx6ull-14x14.dtb` | `38,458 B`（含 `smartgateway,ap3216`） | `38,458 B`（含 `smartgateway,ap3216`） | 节点保留 |
| DTS `linux,cma` 预留池 | `0x14000000`（`320 MiB`） | `0x02000000`（`32 MiB`） | **-288 MiB** |
| 内核 `.config` `=y` / `=m` | `1,330` / `76` | `900` / `48` | `-430` / `-28` |
| 安装内核模块数 / 体积 | `78` 个 / `3,304,232 B` | `48` 个 / `1,131,700 B` | **-65.7%** 体积 |
| 外部 BSP `.ko` 编出验证 | 5 个（`bsp_hello`, `devm_demo`, `dt_match_demo`, `sgw_ap3216`, `sgw_gpio_event`） | 5 个（全量编过） | 全部兼容 |
| 主机编译耗时 | `298 s` | `179 s` | `-40.0%` |

- `fdtget -t x ... /reserved-memory/linux,cma size` 实测分别返回 `14000000` 与 `2000000`；`ap3216@1e` compatible=`smartgateway,ap3216` 在基线与裁剪 DTB 中均存在。

## 2. Buildroot 2020.02 根文件系统实测对比（`build/buildroot-g3/g3_rootfs_metrics.json`）

| 指标 | 原厂基线参考 | G3 裁剪输出 | 说明 |
|---|---|---|---|
| `ext4` 根分区镜像规格 | G0 板端 `p2` 已用 `677 MiB`（`core_defconfig` 声明 `500M`） | `134,217,728 B`（`128 MiB`，`-O ^metadata_csum`） | 可直接放入 `p1`/`p2` A/B 槽位并留足余量 |
| `rootfs.tar` 归档大小 | — | `33,495,040 B`（`31.94 MiB`，`1279` 项） | 含裁剪 Kernel/DTB/Modules + 业务与 OTA |
| 实际文件 payload 总和 | — | `31,401,349 B`（`29.95 MiB`） | 已禁用 `eudev hwdb.bin`（节省 `8.27 MB`） |
| `defconfig` `BR2_PACKAGE_*=y` | `136`（`100ask_imx6ull_pro_ddr512m_systemV_core_defconfig`） | `35`（`bsp/rootfs/g3_trimmed_defconfig`） | **-74.3%** |
| 解析展开后 `.config` `BR2_PACKAGE_*=y` | `315` | `152`（移除 `170` 项，含依赖补齐） | **-51.7%** |

- **关键二进制与 ELF 共享库核验：** 使用 `arm-buildroot-linux-gnueabihf-readelf -d` 检查 `smart_gateway`、`ota_agent`、`service_supervisor`、`sgw_bootctl`、`sgw_watchdog_guard`、`sgw_system_health`、`swupdate`、`adbd`、`gdbserver`，其全部 `14` 个 `NEEDED` 动态库（`libc.so.6`、`libpthread.so.0`、`libssl.so.1.1`、`libcrypto.so.1.1`、`libjson-c.so.4`、`libcurl.so.4`、`libconfig.so.11`、`libz.so.1`、`librsync.so.2`、`libcrypt.so.1`、`libdl.so.2`、`libstdc++.so.6`、`libm.so.6`、`libgcc_s.so.1`）均已核实在 `target/lib/` 或 `target/usr/lib/` 中就绪。
- **必需文件存在性检查：** `boot/zImage`、`boot/100ask_imx6ull-14x14.dtb`、`usr/bin/adbd`、`usr/bin/swupdate`、`usr/bin/sgw_bootctl`、`usr/bin/sgw_watchdog_guard`、`usr/bin/service_supervisor`、`usr/bin/smart_gateway`、`usr/bin/ota_agent`、`sbin/fw_printenv`、`etc/fw_env.config`、`etc/fstab`（`ext4`）、`etc/init.d/S15sgw_ab_guard`、`etc/init.d/S99adbd`、`etc/init.d/S99smartgateway`、`usr/bin/gdbserver` 全部存在于 `rootfs.tar`。

## 3. 主机自动化验证结果

| 测试命令 | 范围 | 最新实测结果 | 状态 |
|---|---|---|---|
| `python3 -m unittest -v tests/test_g3_kernel_rootfs.py` | G3 内核/CMA/DTB/模块、RootFS 体积与必需文件回归 | `2/2 OK`（`0.117 s`） | **PASS (HOST)** |
| `make test` | 网关协议与队列核心 C 单元测试 | `10/10 PASS` | **PASS (HOST)** |
| `make -C bsp/apps/service_supervisor test` | `service_ipc` 与 `service_supervisor` | `58/58 PASS`；`35/35 PASS` | **PASS (HOST)** |
| `make -C bsp/apps/boot_ab test arm-check` | A/B 状态机、健康核查与 ARM ELF 检查 | `ab_state 100/100`、`fw_env 31/31`、`ab_health FAIL=0`、`system_health 195/195`、`health_devices 21/21`、`health_network 5/5`、`arm-check` OK | **PASS (HOST)** |
| `python3 -m unittest -v tests/test_g1_backup_guard.py tests/test_g2_uboot_ab.py tests/test_g2r3_health.py tests/test_g2r3b_gateway_probe.py` | G1/G2 回归与网关探测 | `26/27 OK`：`test_g1_backup_guard` `11/11 OK`，`test_g2_uboot_ab` `7/7 OK`，`test_g2r3_health` `7/7 OK`，`test_g2r3b_gateway_probe` `1/2 OK`（前置已有项 `test_actual_gateway_components_and_no_upstream_pollution` 在当前基线返回 `1: unverified`） | **PARTIAL (前置基线遗留 1 项待修)** |

## 4. 未执行项（NOT RUN / PENDING BOARD）

下列运行时指标严禁估算或填表通过，须待完成 G1 离线备份与外部恢复介质验证后在 i.MX6ULL 真机启动 G3 镜像时实测补齐：

1. **启动耗时实测：** U-Boot 加载 G3 `zImage` + `.dtb` 至内核 `Freeing unused kernel memory` 及 `service_supervisor` / `smart_gateway` 就绪时间。
2. **板端真实内存水位：** 上电空闲后 `cat /proc/meminfo` 中 `MemTotal`、`MemAvailable` 与 `CmaTotal=32768 kB` 实测值。
3. **硬件接口与外设功能回归：** USB ADB（ConfigFS `adbd`）、`FEC` 以太网、RTL8723BU WiFi 关联、蓝牙串口节点与 `sgw_ap3216.ko` / `sgw_gpio_event.ko` 真实 `insmod`/I2C 读写。
4. **SWUpdate 镜像干跑与 eMMC 槽位引导：** 在备份与 SD 卡恢复验证闭环后配合 G4 进行实机刷写与 `WDOG1`/A/B 切换验证。
