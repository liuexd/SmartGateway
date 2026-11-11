# G0-1B: p1/p3 Header and RTC Follow-up

**状态：** BOARD-READ-ONLY-COLLECTED / REVIEWED；G0 未放行  
**平台：** 100ASK i.MX6ULL，Linux 4.9.88，ARMv7  
**证据来源：** 用户提供的开发板 ADB shell 输出  
**安全边界：** 只读执行 blkid、od、cat、date、ls；没有挂载、修复或写入分区。

## 1. p1 / p3 检查

执行：
- `blkid -p -o full /dev/mmcblk1p1`
- `blkid -p -o full /dev/mmcblk1p3`
- `od -Ax -tx1 -N 2048 /dev/mmcblk1p1`
- `od -Ax -tx1 -N 2048 /dev/mmcblk1p3`

结果：
- 两次 blkid 均没有打印文件系统识别信息；命令回显的退出状态为 0。由于 BusyBox 工具版本/实现尚未核实，不能仅凭 exit=0 将其解释为识别成功；当前可确认的是“无识别输出”。
- p1 和 p3 的前 2048 bytes 均为全零（od 以 `*` 折叠重复行，最终偏移为 `000800`，即八进制 2048）。
- `/proc/filesystems` 显示内核支持 ext2、ext3、ext4、vfat 等文件系统；这只说明驱动可用，不说明 p1/p3 中存在对应文件系统。
- p1/p3 当前没有挂载记录。

## 2. 解释边界

前 2048 bytes 全零意味着分区起始区域没有可见的常见文件系统头部特征。对 ext 家族而言，主超级块通常位于分区偏移 1024 bytes，因此该结果也没有显示 ext 主超级块特征。

但这不证明整个分区都是零，也不排除：
- 数据位于更后面的偏移；
- 非常见或厂商自定义格式；
- 分区仅使用部分区域；
- 历史分区表项或保留区域。

MBR 的 bootable 标志只是分区表项属性，不证明 U-Boot 从该分区加载启动文件。MBR type ID 0x0C 也不证明 p3 内部一定是 FAT32。

**当前结论：p1/p3 内容与用途仍未知，禁止格式化、重分区或分配给 A/B。**

## 3. RTC / 系统时间

`date -u` 输出 Tue Jan 6 09:52:30 UTC 1970；`/proc/driver/rtc` 输出 rtc_date=1970-01-06。由此确认板端 RTC 当前时间不正确或未设置。具体原因（RTC 供电、时钟源、驱动初始化、时间同步策略）尚未查明。

影响：
- OTA 日志时间戳不可信；
- 依赖证书有效期的 TLS 连接可能失败；
- 依赖包有效期/签名策略的实现需要明确可信时间来源。

不得仅为消除显示异常而直接写 RTC 或修改寄存器。后续单独检查 RTC 芯片/SoC RTC、供电、驱动和 NTP/网络时间同步策略。

## 4. fstab 与当前根挂载

`/etc/fstab` 声明：
```text
/dev/root / ext2 rw,noauto 0 1
```

但运行时 `/proc/mounts` 显示：
```text
/dev/root / ext4 rw,relatime,data=ordered 0 0
```

Kernel cmdline 指定 `root=/dev/mmcblk1p2 rootwait rw`。当前可确认实际根文件系统是 ext4；fstab 的 ext2 字段与运行时类型不一致。fstab 的该条目还带有 noauto，不能简单推断它负责首次根挂载。根文件系统通常由 Kernel 启动参数和早期用户空间切换/挂载流程建立，需检查 init 脚本、mount 调用和 BusyBox init 配置，确认 fstab 是否被读取以及是否有其他 remount 行为。

## 5. 后续只读调查

1. 在主机端对 p1/p3 做只读镜像采集/哈希前，先评估存储空间和备份路径；不要把镜像写回 eMMC。
2. 在板端可先只读抽查分区中后部偏移（例如 1 MiB、4 MiB、分区中点和末尾附近），但需要明确偏移换算和读取边界。
3. 获取 U-Boot 串口日志与只读 `printenv`、`mmc info`、`mmc part`，确认启动介质和实际加载路径。
4. 查阅 init 脚本、`/etc/inittab`、`/etc/init.d/rcS` 和 mount 相关脚本，追踪根挂载后的用户空间行为。
5. 后续分析 RTC 驱动、设备树节点、硬件供电与网络时间同步。

## 6. G0 决策

**不放行**任何分区修改、格式化、刷写、U-Boot 环境写入或 A/B 槽位分配。继续只读调查与主机端构建准备。
