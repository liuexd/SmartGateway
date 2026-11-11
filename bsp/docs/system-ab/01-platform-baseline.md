# i.MX6ULL Platform and eMMC Baseline

## 1. 记录范围与证据来源

本文件记录用户通过开发板 ADB shell 提供的实测输出，不代表所有恢复条件已经验收。证据包括 uname、/proc/cmdline、/proc/version、/proc/partitions、/dev/mmcblk*、/sys/class/block/mmcblk1*、/proc/mounts、df -h 和 /proc/meminfo。

初次手工采集的命令回显存在交错；后续通过只读脚本 `bsp/tools/collect_system_baseline.sh` 获取了完整报告。脚本报告中的日期为 1970-01-06，说明板端实时时钟/系统时间未正确设置或尚未同步；该时间不能作为可靠的事件时间戳。

## 2. SoC、内核与系统

| 项目 | 实测值 | 说明 |
|---|---|---|
| Linux 主机名 | 100ask | uname |
| Kernel | 4.9.88 | #1 SMP PREEMPT |
| CPU 架构 | armv7l | ARMv7 |
| 控制台 | console=ttymxc0,115200 | cmdline |
| 根设备参数 | root=/dev/mmcblk1p2 rootwait rw | cmdline |
| Kernel 构建主机标识 | ubuntu@ubuntu-VMware-Virtual-Platform | 构建标识，不代表板端运行 Ubuntu |
| Kernel 编译器 | Linaro GCC 6.2.1 | gcc version 6.2.1 20161016 |
| RootFS | ext4 | /proc/mounts 显示 /dev/root 挂载到 / |
| Buildroot | 2020.02 | 沿用既有项目基线，需以构建配置复核 |

## 3. DDR 与 CMA

| 指标 | 实测值 |
|---|---:|
| MemTotal | 500,948 KiB（约 489.2 MiB） |
| MemAvailable | 364,744 KiB（约 356.2 MiB） |
| CmaTotal | 327,680 KiB（320 MiB） |
| CmaFree | 226,248 KiB（约 220.9 MiB） |

CMA 用于满足连续物理内存分配需求，不能简单视为永久不可用的普通内存。CmaFree 是观测时点的空闲量，不代表长期需求。320 MiB 配额列为 DTS、Kernel 配置和 DMA 使用情况审计项；查明 LCD/显示、DMA 及其他设备依赖前不得直接缩小。

## 4. eMMC 设备识别

| 设备 | /proc/partitions #blocks（KiB） | 容量 |
|---|---:|---:|
| mmcblk1 | 3,817,472 | 约 3,728 MiB（3.64 GiB） |
| mmcblk1p1 | 1,024,000 | 1,000 MiB |
| mmcblk1p2 | 1,536,000 | 1,500 MiB |
| mmcblk1p3 | 512,000 | 500 MiB |
| mmcblk1boot0 | 4,096 | 4 MiB |
| mmcblk1boot1 | 4,096 | 4 MiB |
| mmcblk1rpmb | 4,096 | 4 MiB |

Linux 可见 eMMC user area 设备 /dev/mmcblk1 及 p1/p2/p3。设备字段报告为 type=MMC、name=S40004、manfid=0x000001、oemid=0x0100、date=06/2020。boot0、boot1 是 eMMC 专用硬件 boot 分区，不等于 p1/p2/p3；rpmb 是受保护存储区域，本阶段不计划使用。报告有意不采集 serial/CID，避免公开设备唯一标识。boot0/boot1 的 `force_ro` 均为 1，表示当前 Linux sysfs 接口将其标记为只读；这不是对 U-Boot 存储位置的证明。

## 5. 分区扇区几何

Linux sysfs 的 size 和 start 以 512-byte sector 计。

| 分区 | start（sector） | size（sector） | 容量 |
|---|---:|---:|---:|
| p1 | 4,098 | 2,048,000 | 1,000 MiB |
| p2 | 2,052,098 | 3,072,000 | 1,500 MiB |
| p3 | 5,124,098 | 1,024,000 | 500 MiB |
| user area | — | 7,634,944 | 约 3,728 MiB |

按当前 sysfs 数值计算：
- p1 结束扇区（exclusive）：2,052,098。
- p2 结束扇区（exclusive）：5,124,098。
- p3 结束扇区（exclusive）：6,148,098。
- user area 总扇区数：7,634,944。
- p3 之后剩余：1,486,846 sectors，约 726.97 MiB。
- p1 前从 sector 0 到 4,097 有 4,098 sectors，约 2.00 MiB 未包含在 p1 中。

只读 `fdisk -l /dev/mmcblk1` 已确认分区表为 DOS/MBR。p1、p2 的分区类型 ID 均为 0x83（Linux），且两者均带有 MBR bootable 标志；p3 的类型 ID 为 0x0C（Win95 FAT32 LBA）。注意：MBR 类型 ID 不是文件系统探测结果，不能据此认定 p3 一定格式化为 FAT32。上述尾部空间只是根据 Linux 内核暴露的起始扇区和长度做的算术推算，不证明该区域未被厂商元数据、备份数据或其他机制占用。

## 6. 文件系统与占用

/proc/mounts显示根文件系统为 /dev/root，挂载点 /，类型 ext4，rw；/tmp、/run、/dev/shm 使用 tmpfs；ADB FunctionFS 挂载于 /dev/usb-ffs/adb。

| 挂载点 | 文件系统 | Size | Used | Avail | Use% |
|---|---|---:|---:|---:|---:|
| / | /dev/root | 1.5G | 677M | 706M | 49% |
| /dev | devtmpfs | 85M | 0 | 85M | 0% |
| /dev/shm | tmpfs | 245M | 0 | 245M | 0% |
| /tmp | tmpfs | 245M | 376K | 245M | 1% |
| /run | tmpfs | 245M | 500K | 245M | 1% |

当前 p2 是根文件系统所在分区，因为 cmdline 指定 root=/dev/mmcblk1p2，且根文件系统容量约 1.5G。`blkid` 仅识别 p2 的 UUID（491f6117-415d-4f53-88c9-6e0de54deac6）；p1、p3 未返回可识别的文件系统签名。当前 `/proc/mounts` 未显示 p1、p3 挂载。p1/p3 可能包含启动、恢复或厂商数据，必须进一步检查分区头和内容，不能视为空闲。`/boot` 是当前根文件系统目录树中的目录（没有单独挂载记录），其中存在 zImage 和多个 DTB；实际 U-Boot 加载路径仍待确认。

## 7. 已知与待验证

已由本轮输出支持：
- 板端运行 Linux 4.9.88 / ARMv7。
- 根设备参数指向 /dev/mmcblk1p2。
- Linux 识别 mmcblk1 user area、p1/p2/p3、boot0、boot1 和 rpmb。
- p1/p2/p3 起始扇区及长度如上。
- 当前根文件系统为 ext4，使用 677 MiB，可用 706 MiB。
- MemTotal 约 489 MiB，CMA 总量 320 MiB。

尚未确认：
- p1、p3 内部文件系统、标签和用途仍未确认。抽样发现 p1 在相对 512 MiB 与接近末尾处含 QML/XML 风格结构化文本；p3 六个抽样点均为零，但未全盘扫描。
- 原始 MBR 分区表备份及分区表之外的厂商元数据位置。
- RTC/系统时间采样为 1970-01-06；进程列表同时存在 `ntpd -g` 和 `/etc/init.d/S49ntp`，NTP 实际同步状态待查。
- 系统已集成 SWUpdate：`S98swupdate` 根据 `/proc/cmdline` 的 root 字段选择 `rootfs-1` 或 `rootfs-2` 更新集合；当前 p2 按脚本逻辑选择 `rootfs-1`。物理分区映射、启动切换与回滚能力仍待审计。
- `/etc/fw_env.config` 为 `/dev/mmcblk1 0xe0000 0x2000`，即 eMMC user area byte offset 896 KiB、长度 8 KiB；尚未用 `fw_printenv` 验证环境内容，也未确认与 U-Boot 构建配置一致。
- `/etc/fstab` 将 `/` 声明为 ext2 且 `noauto`，但运行时 `/proc/mounts` 为 ext4；早期用户空间挂载/重挂载流程待查。
- p1、p3 的实际文件系统类型、内容和用途；当前 MBR 类型 ID 不能替代文件系统识别。
- p3 后尾部区域是否可用于新增分区。
- U-Boot 本体存储位置；用户空间 `/etc/fw_env.config` 配置为 `/dev/mmcblk1` + `0xE0000`、大小 `0x2000`，但 U-Boot 编译配置是否一致、环境冗余方式仍待确认。
- 原厂完整恢复镜像与恢复流程。
- `S98swupdate` 中固定偏移解析 cmdline、未知 root 默认选择 `rootfs-2`、quoted 参数拼接及 `/bin/sh` 兼容性问题。
- CMA 的实际设备依赖和可裁剪空间。

## 8. G0-1C 补充结果

- p1 在相对 512 MiB 和接近末尾的抽样位置出现 QML/XML 风格结构化文本；p1 不得视为全空或可回收。
- p3 六个分散抽样点（约 1、4、8、128、256 MiB 和末尾附近）各 2 KiB 均为全零；这不等于全分区为空。
- 系统存在 SWUpdate、swupdate-progress、S98swupdate，命令行出现 `rootfs` 与 `rootfs-1` selection。必须先审计现有 SWUpdate 与 bootloader 集成，再决定是否需要新增 A/B 机制。
- `ntpd -g` 正在运行且存在 S49ntp，故 1970 时间问题应进一步核对 NTP 同步状态，不应直接断言无时间同步服务。

## 9. G0-1B 补充结果

- p1、p3 的 `blkid -p -o full` 均无识别输出；回显退出码为 0，但 BusyBox blkid 版本/退出码语义未核实，因此不将其当作识别成功。
- p1、p3 前 2048 bytes 均为全零；这不证明整个分区全零，也不证明分区可回收。
- `/proc/filesystems` 支持 ext2/ext3/ext4/vfat，但仅代表内核支持这些类型。
- RTC 与系统时间均显示 1970-01-06，需单独排查 RTC 供电、时钟源、驱动初始化和网络时间同步。
- `/etc/fstab` 声明 `/dev/root / ext2 rw,noauto`，而运行时根挂载为 ext4。需追踪 BusyBox init/rcS 和 mount 流程；不能假设 fstab 负责首次根挂载。

## 10. 后续只读补充采集

在开发板 Linux shell 执行；工具缺失时保留报错，不安装或修改系统：

```sh
cat /sys/block/mmcblk1/device/type
cat /sys/block/mmcblk1/device/name
# 避免公开设备唯一标识；如确需 CID/CSD，应在本地私有记录中保存并脱敏，不提交公开仓库。

command -v fdisk && fdisk -l /dev/mmcblk1
command -v sfdisk && sfdisk -d /dev/mmcblk1
command -v blkid && blkid
command -v lsblk && lsblk -o NAME,SIZE,FSTYPE,LABEL,UUID,PARTUUID,MOUNTPOINT

cat /proc/self/mountinfo
stat -f /
du -sh /opt /var /usr /lib 2>/dev/null
dmesg | grep -Ei 'cma|reserved memory|Memory:|mmc|EXT4-fs'
```

不要为了运行缺失工具而修改 RootFS。p1/p3 内容检查优先使用只读挂载，并先确认文件系统类型；不得直接执行 fsck 修复、格式化或写入。
