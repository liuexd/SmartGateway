# G0-1 eMMC Read-only Inventory Result

**状态：** HOST-REVIEWED / BOARD-READ-ONLY-COLLECTED；G0 未放行  
**平台：** 100ASK i.MX6ULL，Linux 4.9.88，U-Boot 2017.03（既有记录）  
**证据：** 用户于开发板 ADB shell 执行 `bsp/tools/collect_system_baseline.sh` 后提供的完整报告  
**安全边界：** 本轮只读采集；未挂载未知分区，未写入 eMMC，未修改分区表或 U-Boot 环境。

## 已确认事实

- eMMC user area：7,634,944 个 512-byte sectors，约 3.64 GiB。
- eMMC 非唯一标识字段：type=MMC、name=S40004、manfid=0x000001、oemid=0x0100、date=06/2020。serial/CID 未写入公开记录。
- 分区表：DOS/MBR，由 `fdisk -l` 只读输出确认。
- p1：start=4098，size=2,048,000 sectors（1000 MiB），MBR type=0x83，bootable 标志置位。
- p2：start=2,052,098，size=3,072,000 sectors（1500 MiB），MBR type=0x83，bootable 标志置位；当前 root=/dev/mmcblk1p2，ext4。
- p3：start=5,124,098，size=1,024,000 sectors（500 MiB），MBR type=0x0C。
- p1/p3 当前未出现在 `/proc/mounts`；本轮 `blkid` 仅识别 p2 UUID。
- `/boot` 未单独挂载，当前根文件系统的 /boot 目录中存在 zImage 和多个 DTB。
- boot0/boot1 的 sysfs `force_ro=1`。
- CMA pool 为 320 MiB，启动日志显示创建于 0x8c000000。
- 采集报告时间为 1970-01-06；系统时钟/RTC 尚未核实。

## 关键解释

MBR 分区类型 0x0C 仅表示分区类型标识，不证明 p3 内部一定为 FAT32。p1/p3 的文件系统、数据和用途仍未知。p1/p2 的 MBR bootable 标志也不能单独证明 U-Boot 会从对应分区启动。

p3 之后约 726.97 MiB 是按分区几何计算的尾部空间，不代表可安全使用。

## 下一步

1. 对 p1/p3 执行只读签名探测和分区头十六进制检查。
2. 若识别出文件系统，再制定对应文件系统的只读挂载检查方案；未知格式不得盲目挂载。
3. 在 U-Boot 控制台只读采集 `printenv`、`mmc info`、`mmc part` 和实际启动命令，解释 p1/p2 bootable 标志及 /boot 文件加载路径。
4. 获取准确板型的原厂镜像、源码和恢复流程。

## G0 决策

**不放行分区修改、格式化、刷写、saveenv/fw_setenv 或 A/B 槽位分配。** 继续只读调查和主机端构建准备。
