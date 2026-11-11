# U-Boot Boot Chain and Environment Baseline

## 1. 证据来源

本文件整理用户提供的开发板 U-Boot 串口输出：version、printenv、mmc list、mmc part、help 中与存储/文件系统/启动相关的命令，以及 help bootcount。

U-Boot：U-Boot 2017.03-g8ba4c5bb19-dirty（Jun 10 2026 - 10:24:59 +0800）。编译器为 Linaro GCC 6.2.1，GNU ld 2.27.0.20161019。

## 2. 当前环境变量证据

关键变量：

```text
bootdelay=3
bootdir=/boot
boot_fdt=try
console=ttymxc0
fdt_addr=0x83000000
fdt_file=100ask_imx6ull-14x14.dtb
image=zImage
loadaddr=0x80800000
mmcdev=1
mmcpart=2
mmcroot=/dev/mmcblk1p2 rootwait rw
script=boot.scr
update=yes
```

关键命令变量：

```text
loadbootscript=fatload mmc ${mmcdev}:${mmcpart} ${loadaddr} ${script};
loadfdt=ext2load mmc ${mmcdev}:${mmcpart} ${fdt_addr} ${bootdir}/${fdt_file}
loadimage=ext2load mmc ${mmcdev}:${mmcpart} ${loadaddr} ${bootdir}/${image}
mmcargs=setenv bootargs console=${console},${baudrate} root=${mmcroot}
```

默认 bootcmd：

```text
run updateset;
run findfdt;
run findtee;
mmc dev ${mmcdev};
mmc dev ${mmcdev};
if mmc rescan; then
  if run loadbootscript; then
    run bootscript;
  else
    if run loadimage; then
      run mmcboot;
    else
      run netboot;
    fi;
  fi;
else
  run netboot;
fi
```

mmcboot 通过 mmcargs 设置 bootargs；当 boot_fdt=yes 或 try 时执行 loadfdt；loadfdt 使用 ext2load 从 mmc 1:2 的 /boot/ 路径加载 DTB；loadimage 从同一分区加载 /boot/zImage；最终使用 bootz 启动 zImage 并传入 DTB 地址。

当前配置将 mmcdev 指向 1、mmcpart 指向 2，mmcroot 指向 /dev/mmcblk1p2。Linux cmdline 实测同样是 console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw。

因此，当前启动配置指向 eMMC 第 2 个分区作为 Kernel/DTB 文件加载分区和根文件系统分区。实际文件加载成功的历史串口记录见既有 D01 基线文档；本轮 printenv 提供了对应环境变量证据。

## 3. U-Boot 命令能力与限制

help 输出显示存在 mmc、mmcinfo、ext2load、ext4load、ext2ls、ext4ls、fatload、bootz、saveenv、setenv 等命令。这说明该构建列出了 ext4 文件系统加载命令；后续仍须实测 U-Boot 对目标分区的读取能力，不能只凭 help 列表认定 A/B 槽位已验证。

### bootcount

help bootcount 返回 Unknown command 'bootcount'。这只说明没有名为 bootcount 的独立交互命令，不能单独证明 U-Boot 未启用 CONFIG_BOOTCOUNT_LIMIT 或环境变量 bootcount/bootlimit 机制。需通过匹配的 U-Boot .config、源码及启动脚本确认。

### MMC 交互式探测异常

本轮 mmc list 显示 FSL_SDHC: 0 和 FSL_SDHC: 1 (eMMC)。随后 mmc part 返回 MMC: no card present、mmc_init: -123。

该结果与 Linux 已识别 eMMC、且 U-Boot printenv 配置从 mmc 1 启动的事实需要进一步核对。可能与执行 mmc part 前未显式选择设备/重新扫描、命令状态或控制器初始化有关；目前不能据此断定 eMMC 不可访问，也不能断定分区表损坏。

下一步仅做只读交互验证：

```text
mmc list
mmc dev 1
mmc rescan
mmc info
mmc part
```

若仍失败，记录完整串口输出并停止，不尝试写入或擦除。

## 4. 环境变量持久化尚未查明

printenv 显示 Environment size: 2765/8188 bytes。该信息表示当前环境变量内容及环境区容量信息，但不揭示环境存储介质、偏移、擦除大小或是否启用冗余环境。

虽然命令列表包含 saveenv，当前阶段禁止执行它。

必须从与板端完全匹配的 U-Boot 源码、defconfig、编译配置和板级代码中确认：
- CONFIG_ENV_IS_IN_MMC / SPI_FLASH / NAND 等实际选项；
- 环境存储设备与硬件分区；
- offset、size、sector size；
- 是否配置 redundant environment；
- U-Boot 本体实际存放位置和恢复方式。

不得猜测偏移，也不得使用未经核实的 fw_env.config。

## 5. A/B 启动设计影响

当前启动脚本使用固定 mmcpart=2 和 mmcroot=/dev/mmcblk1p2。A/B 方案需要将这两个固定值改为由持久化启动状态选择的槽位参数，并确保：
1. Kernel 与 DTB 从同一目标槽位加载；
2. root= 指向同一槽位对应的根文件系统；
3. pending 槽位只有在 Linux 健康确认后才标记为 confirmed；
4. 启动次数和回退状态在复位后可靠保存；
5. 环境元数据更新具备断电一致性；
6. 原有已确认槽位在新槽确认前不被覆盖。

第一阶段不更新 U-Boot 本体。若现有 U-Boot 环境不具备可靠冗余能力，需要评估独立冗余 Boot Control Metadata，而不是直接写入未知保留区域。

## 6. 只读核对清单

- [ ] 在 U-Boot 执行 mmc dev 1、mmc rescan、mmc info、mmc part 并保存结果。
- [ ] 获取匹配 U-Boot 源码及 .config。
- [ ] 确认 U-Boot 环境存储介质、offset、size 和冗余配置。
- [ ] 确认 U-Boot 本体存储位置。
- [ ] 确认原厂恢复工具能否恢复 U-Boot 和环境。
- [ ] 验证原有启动脚本、boot.scr 和 fallback 网络启动行为。
- [ ] 只读确认 p2 中 /boot/zImage、DTB 和 boot.scr 的实际存在情况。
