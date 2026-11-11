# i.MX6ULL Platform and eMMC Baseline

## 1. 记录范围与证据来源

本文件记录用户通过开发板 ADB shell 提供的实测输出，不代表所有恢复条件已经验收。证据包括 uname、/proc/cmdline、/proc/version、/proc/partitions、/dev/mmcblk*、/sys/class/block/mmcblk1*、/proc/mounts、df -h 和 /proc/meminfo。

本轮粘贴的命令在开头有终端输入/回显交错，且 dmesg grep 未提供可读结果。因此不把本轮 dmesg 当作有效证据；后续需单独保存原始输出。

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

Linux 可见 eMMC user area 设备 /dev/mmcblk1 及 p1/p2/p3。boot0、boot1 是 eMMC 专用硬件 boot 分区，不等于 p1/p2/p3；rpmb 是受保护存储区域，本阶段不计划使用。

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

上述尾部空间只是根据 Linux 内核暴露的起始扇区和长度做的算术推算，不证明分区表允许在该位置创建新分区，也不证明该区域未被厂商元数据、备份数据或其他机制占用。必须使用分区表解析工具和原厂资料交叉核验。

## 6. 文件系统与占用

/ proc/mounts（实际路径为 /proc/mounts）显示根文件系统为 /dev/root，挂载点 /，类型 ext4，rw；/tmp、/run、/dev/shm 使用 tmpfs；ADB FunctionFS 挂载于 /dev/usb-ffs/adb。

| 挂载点 | 文件系统 | Size | Used | Avail | Use% |
|---|---|---:|---:|---:|---:|
| / | /dev/root | 1.5G | 677M | 706M | 49% |
| /dev | devtmpfs | 85M | 0 | 85M | 0% |
| /dev/shm | tmpfs | 245M | 0 | 245M | 0% |
| /tmp | tmpfs | 245M | 376K | 245M | 1% |
| /run | tmpfs | 245M | 500K | 245M | 1% |

当前 p2 是根文件系统所在分区，因为 cmdline 指定 root=/dev/mmcblk1p2，且根文件系统容量约 1.5G。p1、p3 用途尚未确认。

## 7. 已知与待验证

已由本轮输出支持：
- 板端运行 Linux 4.9.88 / ARMv7。
- 根设备参数指向 /dev/mmcblk1p2。
- Linux 识别 mmcblk1 user area、p1/p2/p3、boot0、boot1 和 rpmb。
- p1/p2/p3 起始扇区及长度如上。
- 当前根文件系统为 ext4，使用 677 MiB，可用 706 MiB。
- MemTotal 约 489 MiB，CMA 总量 320 MiB。

尚未确认：
- p1、p3 内部文件系统、标签、文件内容和用途。
- eMMC 分区表类型（DOS/MBR 或 GPT）及原始分区表备份。
- p3 后尾部区域是否可用于新增分区。
- U-Boot 本体和环境变量的存储位置、偏移、大小及冗余方式。
- 原厂完整恢复镜像与恢复流程。
- CMA 的实际设备依赖和可裁剪空间。

## 8. 后续只读补充采集

在开发板 Linux shell 执行；工具缺失时保留报错，不安装或修改系统：

```sh
cat /sys/block/mmcblk1/device/type
cat /sys/block/mmcblk1/device/name
cat /sys/block/mmcblk1/device/cid
cat /sys/block/mmcblk1/device/csd

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
