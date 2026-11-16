# G3 Buildroot 用户空间裁剪与依赖审计清单

## 1. 基线与外部工具链策略

- **原始厂商基线配置：** `Buildroot_2020.02.x/configs/100ask_imx6ull_pro_ddr512m_systemV_core_defconfig`（`defconfig` 含 `136` 个 `BR2_PACKAGE_*=y`，经 Buildroot Kconfig 解析展开后共 `315` 个 `BR2_PACKAGE_*=y` 启用项；ext4 镜像声明 `500M`；G0 板端实测 `p2` 已用空间 `677 MiB`）。
- **G3 裁剪目标配置：** [g3_trimmed_defconfig](g3_trimmed_defconfig)（`defconfig` 含 `35` 个 `BR2_PACKAGE_*=y`，解析展开后共 `152` 个启用项，净移除 `170` 个原厂包及子选项，完整包级差异见 [g3_trimmed_vs_core_packages.diff](g3_trimmed_vs_core_packages.diff)）。构建复用 VM 已安装的 Buildroot 2020.02 外部工具链 SDK（GCC 7.5.0 / glibc 2.30 / Linux 4.9.88 headers）与本地 `dl/` 源码缓存。
- **RootFS 镜像规格：** `ext4`（`-O ^metadata_csum` 兼容 U-Boot 2017.03 `ext2load`/`ext4load` 与旧工具），镜像文件大小收敛至 `128 MiB`（`134,217,728 B`），其中实际文件 payload 为 `31,401,349 B`（约 `29.95 MiB`），在 `p1=1000 MiB` / `p2=1500 MiB` 槽位内预留充裕系统裕量与运行日志空间。

## 2. 软件包与 Overlay 保留 / 裁剪矩阵

| 类别 | 组件 / 包 | 决策 | 技术依据 |
|---|---|---|---|
| **核心启动与设备** | `busybox`、`init_sysv`、`eudev`、`bash`、`start-stop-daemon`、`kmod-tools` | **保留(精简)** | 保留 SysV `/etc/init.d/rcS`、热插拔与驱动加载；关闭 `BR2_PACKAGE_EUDEV_ENABLE_HWDB`，移除 `8.27 MB` `hwdb.bin` 与 `hwdb.d/` |
| **USB ADB 调试** | `android-tools` (`adbd`)、`61-usb-adbd.rules`、`S99adbd`、`usb_config` | **保留** | 通过 ConfigFS FunctionFS 提供免网络 USB ADB 调试通道 |
| **eMMC / 分区 / FS** | `e2fsprogs` (`resize2fs`)、`dosfstools`、`mmc-utils`、`util-linux` (`libfdisk`/`libmount`) | **保留** | G1/G4/G5 离线检查、ext4 修复、FAT32 `p3` 检查及 SWUpdate 依赖 |
| **系统/应用 OTA** | `swupdate`、`librsync`、`libconfig`、`json-c`、`openssl`、`zlib`、`libcurl`、`ca-certificates`、`fw_printenv`/`fw_setenv`（冗余版） | **保留/替换** | `librsync` 满足 Buildroot `swupdate` 默认 `rdiff_handler` 链接需求；`fw_printenv` 由 `build/uboot-ab/arm/fw_printenv` 覆盖匹配 `0xE0000`+`0xE2000` 冗余头部；集成 `sgw_bootctl`、`sgw_watchdog_guard`、`sgw_system_health`、`service_supervisor`、`smart_gateway`、`ota_agent` |
| **网络与 WiFi** | `iproute2`、`wpa_supplicant` (`cli`/`passphrase`)、`iw`、`wireless-tools`、`openssh`、`ntp` | **保留** | FEC 以太网、RTL8723BU/WPA2 WiFi Worker、SSH 维护及 NTP 对时 |
| **硬件与 G6 调试** | `i2c-tools`、`gdb` (`gdbserver` only)、`strace` | **保留(精简)** | 目标板只保留 `gdbserver` 与 `strace`，移除目标板全量 `gdb` 调试器及 host-gdb python |
| **图形/UI/多媒体** | `llvm`、`mesa3d`、`qt5`、`libdrm`、`fb-test-app`/`fbset`/`fbdump`、`psplash`、`usr/share/lvgl` (17M)、`S05lvgl` | **裁剪移除** | SmartGateway 为无头网关节点；移除后消除桌面图形栈体积与启动 splash 竞争 |
| **音频/摄像头/流媒体** | `pulseaudio`、`alsa-utils`、`alure`、`speexdsp`、`mjpg-streamer`、`etc/pulse` | **裁剪移除** | 网关业务无音频播放/采集与视频推流需求 |
| **冗余无线/协议栈** | `bluez5_utils`、`bluez-alsa`、`bluez-tools`、`openobex`、`pppd`、`etc/bluetooth` | **裁剪移除** | 源码审计 `gateway/bluetooth_worker.c` 证实蓝牙节点走串口链路，不依赖 BlueZ 协议栈 |
| **冗余工具与库** | `opkg`、`gnupg2`、`boost`、`mosquitto`、`can-utils`、`libxml2`、`coreutils`、`samba`、`php.ini`、`S50telnet` | **裁剪移除** | 与 SWUpdate/BusyBox 重叠或为未使用中间件；保留 `sshd` 去掉明文 `telnet` |
| **启动脚本修复** | `etc/fstab`、去掉 `S35resize`、去掉旧 `S98swupdate` | **修正** | `fstab` 根类型由 `ext2` 修正为 `ext4`；去除旧 `S35resize`（避免 A/B 固定槽位误扩）与旧 `S98swupdate`（留待 G4 接入 A/B 系统升级状态机） |
