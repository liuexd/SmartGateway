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


## 7. G0-1C：分区抽样与现有 SWUpdate 线索

**证据来源：** 后续开发板只读 shell 输出。抽样命令通过 `dd if=/dev/mmcblk1pX` 读取，每次 4×512-byte sectors，再由 `od` 显示；未向 eMMC 写入。

### p1 抽样结果

| p1 相对 sector | 相对偏移 | 结果 |
|---:|---:|---|
| 2,048 | 1 MiB | 2 KiB 全零 |
| 8,192 | 4 MiB | 2 KiB 全零 |
| 16,384 | 8 MiB | 2 KiB 全零 |
| 1,048,576 | 512 MiB | 出现可读 ASCII/QML 风格文本 |
| 2,047,996 | 分区末尾前 4 sectors | 出现 XML 风格 `internalNodes`、`leafValues` 等文本 |

512 MiB 处可见 `edit.anchors.*Margin`、`Component.onCompleted`、`StyleItem`、`elementType: "spinbox"` 等字符串，具有 Qt Quick/QML 风格。分区末尾附近可见 `<internalNodes>`、`<leafValues>` 及数值数据。只能确认这些采样块含结构化文本；不能据此确定文件名、格式、所属软件或是否为可执行启动内容。

### p3 抽样结果

相对 sector 2,048、8,192、16,384、262,144、524,288、1,023,996（约 1、4、8、128、256 MiB 及分区末尾附近）对应的每个 2 KiB 样本均为全零。该结果只覆盖抽样点，不证明 p3 全分区为空。

### 工具版本采集限制

`blkid -V` 未产生可见版本信息；`busybox` 命令返回 not found。因此不能将系统环境简单描述为可直接调用 BusyBox 多调用程序，也不能根据先前 blkid 回显 exit=0 推导识别成功。后续应通过 `command -v blkid`、`readlink -f "$(command -v blkid)"`、`blkid --help` 等只读方式确认实际实现（命令不存在时保留错误）。

### 现有 OTA / A-B 机制的关键线索

开发板进程列表显示：
- `/usr/bin/swupdate-progress -w -r`
- `/usr/bin/swupdate -f /etc/swupdate/swupdate.cfg -L -e rootfs,rootfs-1 -u -c 1 -i BrImx6ullpro_01-8b0806700`（进程列表中出现两个 swupdate 进程条目）

同时，`/etc/init.d/` 存在 `S98swupdate`。这表明当前系统已经集成 SWUpdate，且命令行出现 `rootfs` 与 `rootfs-1` 两个 software selection 名称。**这是调查现有双 RootFS / OTA 布局的强线索，但尚不能证明 A/B 已正确配置、当前槽位映射为何，或升级/回滚可用。**

在设计新 A/B 方案前，必须先只读检查：
- `/etc/swupdate/swupdate.cfg`
- `/etc/swupdate/*`（仅列目录与查看非敏感配置；证书/私钥内容不得输出或提交）
- `/etc/init.d/S98swupdate`
- `/etc/fw_env.config`（如存在，注意记录存储偏移但不执行 fw_setenv）
- `fw_printenv`（只读；若工具存在且输出含敏感信息，先脱敏）
- U-Boot 的 bootcmd/bootargs 与当前启动槽位标识

需确认 SWUpdate 的 software collections、rootfs/rootfs-1 对应设备、写入方式、bootloader integration、bootloader environment 变量、健康检查和回滚路径。禁止运行升级命令或写入 U-Boot 环境进行探测。

### RTC / NTP 线索修正

进程列表中存在 `/usr/sbin/ntpd -g`，且 init 脚本目录存在 `S49ntp`。因此目前只能确认采样时 RTC/系统时间为 1970-01-06，同时 NTP daemon 正在运行；不能据此断言系统没有时间同步服务或 NTP 一定失败。需要检查 NTP 配置、同步状态和启动日志。

## 8. 更新后的 G0 决策

- p1 已在 512 MiB 和接近末尾的抽样点发现结构化数据，不能视为空分区。
- p3 的六个抽样点均为零，但尚未进行全分区扫描，也未确认厂商用途。
- 系统已集成 SWUpdate，且出现 `rootfs`、`rootfs-1` selection 名称；必须先审计现有 OTA 设计，避免重复造轮子或破坏已有升级布局。
- RTC 时间异常需要结合正在运行的 ntpd 和 `S49ntp` 一并调查。
- G0 仍未放行。禁止格式化、重分区、刷写、执行 SWUpdate 升级或修改 U-Boot 环境。
