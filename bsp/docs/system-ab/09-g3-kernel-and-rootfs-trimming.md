# G3：Linux 4.9.88 内核与 Buildroot RootFS 裁剪定制

> 本文档对应阶段 **G3-0（裁剪前内核基线复现）** 与 **G3（内核 + DTS CMA + Buildroot RootFS 裁剪定制）**。当前阶段仅完成主机侧独立目录交叉构建与镜像/包结构回归校验（`HOST-TESTED`），未向开发板 eMMC 写入任何分区或镜像。

## 1. 设计原则与不可破坏边界

依据 G0–G2 已确立的平台基线与后续 G4/G5/G6 需求，裁剪必须严格保留以下能力：

1. **现有自研 BSP 驱动全量可编可载：** `bsp_hello.ko`、`sgw_ap3216.ko`、`sgw_gpio_event.ko`、`devm_demo.ko`、`dt_match_demo.ko`，以及 `I2C1` 下 `ap3216@1e`（`smartgateway,ap3216`）设备树节点。
2. **USB ADB 免网调试通道：** `CONFIG_USB_CONFIGFS_F_FS=y`、`adbd`、`61-usb-adbd.rules`、`usb_config`、`S99adbd`。
3. **存储与系统启动链路：** `MMC` / `SDHCI_ESDHC_IMX`（`usdhc1`/`usdhc2`）、`EXT4_FS`、`VFAT_FS`、`IMX2_WDT`（`wdog1`）、`e2fsprogs`、`dosfstools`、`mmc-utils`、SysV init、`eudev`。
4. **网络与无线通道：** `FEC` 有线以太网、`CFG80211`/`MAC80211`、`USB_NET`、`RTL8723BU`/WPA2（`wpa_supplicant`、`iw`、`wireless-tools`）、`openssh`、`ntp`。
5. **A/B 系统确认与 OTA 运行时：** `sgw_bootctl`、`sgw_watchdog_guard`、`sgw_system_health`、`S15sgw_ab_guard`、`fw_printenv`/`fw_setenv`（G2 冗余 env 版本 + `/etc/fw_env.config`）、`service_supervisor`、`smart_gateway`、`ota_agent`、`swupdate` 及其共享库依赖（`openssl`/`libcurl`/`libconfig`/`json-c`/`zlib`/`librsync`/`ca-certificates`）。
6. **G6 性能分析与远程调试：** 内核保留 `CONFIG_PERF_EVENTS=y`、`CONFIG_HW_PERF_EVENTS=y`、`CONFIG_KALLSYMS=y`；RootFS 保留目标端 `gdbserver` 与 `strace`。

同时保证上游参考源码树 `~/Desktop/Linux-4.9.88` 与 `~/Desktop/Buildroot_2020.02.x` 保持只读不污染，所有构建均在独立 `build/` 工作目录通过补丁、配置片段与 `post_build` 钩子完成。

## 2. G3-0 与 G3 内核及设备树工程实现

### 2.1 隔离构建流水线（`build_kernel_g3.sh`）

脚本 [bsp/kernel/build_kernel_g3.sh](../../kernel/build_kernel_g3.sh) 将源码硬链接/拷贝到独立的 `build/kernel-g3/{baseline,trimmed}/src` 中执行编译：

- **G3-0 裁剪前基线（`baseline`）：** 仅应用既有 `ap3216@1e` DTS 补丁 [bsp/dts/patches/0001-add-ap3216-device-tree-node.patch](../../dts/patches/0001-add-ap3216-device-tree-node.patch)，采用原厂 `100ask_imx6ull_defconfig` 完整编出 `zImage`、`100ask_imx6ull-14x14.dtb`、内核模块及 5 个外部 `.ko`，作为严格同工具链下的对照基线。
- **G3 裁剪版（`trimmed`）：** 在上述基础上叠加 CMA 设备树裁剪补丁 [0001-dts-trim-cma-from-320m-to-32m.patch](../../kernel/patches/0001-dts-trim-cma-from-320m-to-32m.patch) 与 Kconfig 裁剪片段 [g3_trimmed.fragment](../../kernel/g3_trimmed.fragment)，输出统一差异文件 [g3_kernel_config.diff](../../kernel/g3_kernel_config.diff) 与量化 JSON 报告 `build/kernel-g3/g3_kernel_metrics.json`。
- **主机 GCC 兼容处理：** 对高版本主机 GCC 14/15，通过传递 `HOSTCC="gcc -fcommon"` 并在独立构建拷贝上注入 `extern YYLTYPE yylloc`（`scripts/dtc/dtc-lexer.lex.c_shipped`）完成 dtc 宿主编译，无需修改上游源码树。

### 2.2 DTS CMA 裁剪：320 MiB → 32 MiB

G0 实测显示板载内存为 512 MiB（`MemTotal = 500,948 KiB`），原厂 `100ask_imx6ull-14x14.dts` 为显示/多媒体保留了 `linux,cma { size = <0x14000000>; }`（`320 MiB`，占整机内存 64%）。由于本网关为无头节点且裁剪掉了 DRM/FB/Vivante GPU，补丁 [0001-dts-trim-cma-from-320m-to-32m.patch](../../kernel/patches/0001-dts-trim-cma-from-320m-to-32m.patch) 将其缩减为 `<0x02000000>`（`32 MiB`），释放 `288 MiB` 给通用页帧与内核/应用缓存。

### 2.3 Kconfig 四主题裁剪策略（[g3_trimmed.fragment](../../kernel/g3_trimmed.fragment)）

1. **多 SoC 与冗余 SMP 清理：** 关闭 `SOC_IMX50`/`51`/`53`/`6Q`/`6SL`/`6SX`/`7D`/`VF610` 及 `SMP`（i.MX6ULL 为单核 Cortex-A7），保留 `SOC_IMX6UL=y`。
2. **显示、音频、触摸与 GPU 清理：** 关闭 `DRM`（含 `DRM_VIVANTE`）、`FB`（`FB_MXC_*`）、`BACKLIGHT_CLASS_DEVICE`、`LOGO`、`SOUND`、`SND_*`、`MEDIA_SUPPORT`、`INPUT_TOUCHSCREEN`。
3. **重型追踪剔除与调试按需保留：** 关闭 `FTRACE`、`FUNCTION_TRACER`、`DEBUG_INFO` 等，保留 `PERF_EVENTS`、`HW_PERF_EVENTS`、`KALLSYMS`、`PRINTK`。
4. **未使用存储/无线/文件系统清理：** 关闭 `MTD`、`UBIFS_FS`、`JFFS2_FS`、`BT`（串口蓝牙无需内核蓝牙协议栈）、`BCMDHD`、`NFS_FS`、`CIFS`、`STAGING`（含 `ION`），保留 `EXT4_FS`、`VFAT_FS`、`MMC`、`FEC`、`CFG80211`/`MAC80211`/`RTL8723BU`、`USB_CONFIGFS_F_FS`。

## 3. Buildroot 2020.02 RootFS 裁剪与 Overlay 修复

完整选包矩阵见 [bsp/rootfs/g3_package_audit.md](../../rootfs/g3_package_audit.md)，核心设计如下：

1. **外部工具链复用与离线可重复构建：** [g3_trimmed_defconfig](../../rootfs/g3_trimmed_defconfig) 指向已验证的 Buildroot 2020.02 外部 SDK（GCC 7.5.0 / glibc 2.30 / Linux 4.9.88 headers），直接复用 `Buildroot_2020.02.x/dl` 缓存。通过隔离目录补丁 [bsp/rootfs/br_patches/](../../rootfs/br_patches/)（修复 glibc >=2.33 `m4` `SIGSTKSZ` 与 `fakeroot` `_STAT_VER`）及仅在构建脚本 [build_rootfs_g3.sh](../../rootfs/build_rootfs_g3.sh) 内生成的无空格包装器 `host-gcc-gnu11` / `host-gxx-compat`（`-include cstdint`）与 `ac_cv_path_mkdir=/bin/mkdir`，在不动主机 Buildroot 上游源码的前提下完成全量编译。
2. **镜像格式与空间控制：** 采用 `ext4`（`-O ^metadata_csum`）`128 MiB` 镜像与 `rootfs.tar` 归档，并显式关闭 `BR2_PACKAGE_EUDEV_ENABLE_HWDB`，去除嵌入式网关不需要的 `8.27 MB` `hwdb.bin` 与 `hwdb.d/`。
3. **`post_build_g3.sh` 根文件系统定制修复：**
   - 将 `/etc/fstab` 根挂载类型由厂商遗留的 `ext2` 修正为与实际一致的 `ext4`。
   - 清理 `/etc/profile` 中的 Qt 环境变量及 `rcS` 中的 `psplash` 调用；去掉旧 `S35resize`、`S50telnet`、`S05lvgl` 与旧 `S98swupdate`。
   - 注入 G3 裁剪内核 `boot/zImage`、设备树 `boot/100ask_imx6ull-14x14.dtb`、内核模块及 5 个外部 `.ko`（`lib/modules/<kver>/extra/`），清空悬空 `build`/`source` 符号链接。
   - 集成 G2 冗余版 `sbin/fw_printenv` -> `fw_setenv`、`/etc/fw_env.config`、`sgw_bootctl`、`sgw_watchdog_guard`、`sgw_system_health`、`S15sgw_ab_guard`、`service_supervisor`、`smart_gateway`、`ota_agent` 与 `S99smartgateway`。
