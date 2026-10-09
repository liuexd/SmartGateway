# D01 · i.MX6ULL 硬件、启动与文件系统基线

- **记录日期**：2026-10-09
- **阶段状态**：系统识别与引导日志采样完成；原厂镜像与恢复验收**待完成**。
- **证据来源**：用户在实际开发板执行的命令，以及板载 USB 调试串口输出；并非离线仿真。
- **注意**：以下“EVK”是设备树和 U-Boot 上报的 model，不据此直接认定 PCB 是 NXP 官方 EVK。

## 1. 硬件信息

| 字段 | 观察结果 | 依据 |
| --- | --- | --- |
| SoC | Freescale / NXP i.MX6ULL rev1.1 | U-Boot `CPU:` |
| CPU | ARM Cortex-A7，ARMv7，单核 | `/proc/cpuinfo`; `dmesg` 显示 1 CPU |
| DDR | 512 MiB | U-Boot `DRAM: 512 MiB`；内核 524288 KiB |
| eMMC 类型 | `MMC` | `/sys/block/mmcblk1/device/type` |
| eMMC 型号字符串 | `S40004` | `/sys/block/mmcblk1/device/name` |
| eMMC 容量 | 3.64 GiB | `dmesg` / `/proc/partitions` |
| Linux 设备树 model | Freescale i.MX6 ULL 14x14 EVK Board | `/proc/device-tree/model` |
| 设备树 compatible | `fsl,imx6ull-14x14-evk`, `fsl,imx6ull` | `/proc/device-tree/compatible` |
| 外设及 PCB 版本 | 尚未核对 | 待查原厂资料及丝印 |

## 2. 软件与根文件系统

| 字段 | 观察结果 |
| --- | --- |
| U-Boot | `2017.03-g8ba4c5bb19-dirty`，构建于 2026-06-10 |
| Kernel | `4.9.88`，SMP PREEMPT，ARMv7 |
| 内核构建编译器 | Linaro GCC 6.2.1（build banner） |
| 内核构建主机标识 | `ubuntu@ubuntu-VMware-Virtual-Platform`；不代表板上运行 Ubuntu |
| 用户空间系统 | Buildroot `2020.02-g65177d4` |
| Kernel cmdline | `console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw` |
| rootfs | eMMC 普通分区 `/dev/mmcblk1p2`，ext4，挂载点 `/` |
| `/proc/mounts` 设备来源显示 | `/dev/root`；根设备实际分区由 cmdline 和内核挂载日志确认 |
| `/tmp` | `tmpfs`，当次查看时上限约 245 MiB，已用约 280 KiB |
| Swap | 0，`/proc/swaps` 只有表头 |

**eMMC 分区（当次观察）**

- `/dev/mmcblk1`：总容量约 3.64 GiB（用户区）。
- `mmcblk1p1`：约 1000 MiB，用途待核对。
- `mmcblk1p2`：约 1500 MiB，已确认是 ext4 根文件系统。
- `mmcblk1p3`：约 500 MiB，用途待核对。
- `mmcblk1boot0`、`mmcblk1boot1`：各约 4 MiB，eMMC 专用 boot 硬件区。
- `mmcblk1rpmb`：约 4 MiB，受保护区。
- **注意**：U-Boot 输出的 `mmc1(part 0)` 不是 Linux 的 `mmcblk1p1`；不能据此判断 U-Boot 镜像保存在哪个扇区或 boot 分区。

## 3. 内存观测与解释

- `free -m` 当次输出：MemTotal 489 MiB；used 128 MiB；free 280 MiB；buff/cache 80 MiB；available 360 MiB；Swap 0。
- `dmesg`：`Reserved memory: created CMA memory pool at 0x8c000000, size 320 MiB`。
- `dmesg`：`Memory: 172244K/524288K available ... 24364K reserved, 327680K cma-reserved`（这是**启动早期**的统计，不等于运行中 `free` 的值）。
- CMA 预留区域并非固定永久全数占用；实际是否需要这么大的 CMA，需要在后续 DTS / 驱动验证阶段分析。
- `/tmp` 的 `df -h` Size 245 MiB 是 tmpfs 配额，不是预先独占 245 MiB RAM。

## 4. 启动链路：已证实与待证实

1. 已通过板载 USB 串口看到 `U-Boot 2017.03` 启动 banner。
2. U-Boot 输出 `MMC: FSL_SDHC: 0, FSL_SDHC: 1`、`mmc1(part 0) is current device`、`Booting from mmc ...`。
3. U-Boot 读取了 8,873,376 bytes 内核映像与 38,370 bytes 设备树文件；镜像 DDR 地址为 `0x80800000`，DTB 地址为 `0x83000000`。
4. U-Boot 运行时修改 DTB 中 `/soc/aips-bus@02200000/epdc@0228c000` 的 `status` 为 disabled；这不等于修改了存储介质里的原文件。
5. 出现 `Starting kernel ...`；Linux 接下来使用 `root=/dev/mmcblk1p2`，成功挂载 ext4 根文件系统并进入 Buildroot 用户空间。
6. **待确认**：U-Boot 本体实际存储区、内核与 DTB 源分区/文件路径、启动脚本/环境变量、恢复镜像匹配情况。

### 只读证据命令

```sh
cat /proc/cmdline
cat /sys/class/block/mmcblk1p2/dev
cat /proc/self/mountinfo
cat /sys/block/mmcblk1/device/type
cat /sys/block/mmcblk1/device/name
dmesg | grep -Ei 'mmc|EXT4-fs|Linux version|Machine model'
```

## 5. 需要跟进的启动提示（未判定为故障根因）

| 日志 | 当前认识 | 下阶段处理 |
| --- | --- | --- |
| `## Error: "findtee" not defined` | 启动脚本引用未定义的 `findtee` | 只读检查 `printenv`，不执行 `saveenv` |
| `** Unrecognized filesystem type **` | 有一次文件系统识别/访问尝试失败；实际镜像仍成功加载 | 核对 bootcmd 和镜像布局 |
| `EXT4-fs ... couldn't mount as ext3` | ext3 尝试不兼容；随后 ext4 成功挂载 | 不按严重故障处理 |
| `EXT4-fs ... recovery complete` | 本次挂载执行了日志恢复 | 记录并在正常重启后观察 |
| `/cpus/cpu@0 missing clock-frequency property` | DTS 相关提示 | G2 核查 DT |
| `Duplicate name in lcdif ... display#1` | DT 节点重复名称提示 | G2 核查 DT |
| `driver_unregister+0x54/0x58` WARNING | reboot 关机卸载驱动阶段出现 WARNING | 保存完整 trace，后续驱动生命周期分析 |
| `Reset cause: WDOG` | 一次主动 reboot 后报告看门狗复位来源 | 结合平台复位实现核对；不直接认定故障 |

## 6. D01 验收与下一步

- [x] 通过 ADB Shell 读取内核与硬件信息。
- [x] 确认 rootfs、eMMC、procfs/tmpfs 等挂载关系。
- [x] 通过板载 USB 调试串口读取 U-Boot 启动阶段输出。
- [x] 记录内核、Buildroot、U-Boot 版本和日志摘录。
- [ ] 确认真实 PCB 板型和硬件版本。
- [ ] 逐项核对现有厂商资料包与板型、系统版本的匹配关系。
- [ ] 在电脑保存一份**完整、原始**串口启动日志（当前仓库只保存会话摘录）。
- [ ] 核对原厂镜像、启动介质与**可靠恢复方案**；在验证之前不做破坏性操作。
- [ ] 对照主计划完成 D01 退出验收。

当日采集的片段分别见 `../logs/D01-uboot-excerpt.log` 与 `../logs/D01-kernel-excerpt.log`。未经测试的事项不得写成已通过。
