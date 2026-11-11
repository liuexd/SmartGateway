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


## 9. G0-2A：原厂 SWUpdate 与 U-Boot 环境配置初查

**证据来源：** 开发板 ADB shell 输出；本轮仅读取配置、进程列表和文件目录，没有执行升级或写入 U-Boot 环境。

### SWUpdate 文件与服务

`/etc/swupdate/` 包含：
- `swupdate.cfg`（451 bytes）
- `postupdate.sh`（116 bytes）
- `swupdate-public.pem`（471 bytes）

目录列表已确认，但本轮配置文件的 `cat` 内容尚未成功采集；先前的 `grep` 命令回显发生输入/终端交错，不能据此判断配置为空。公钥文件不等于私钥，不应将任何私钥或设备凭据提交到公开仓库。

`/etc/init.d/S98swupdate` 显示：
- 读取 `/proc/cmdline`，通过固定位置参数展开 `${LINE:28:14}` 提取 root 设备字符串；
- 若结果等于 `/dev/mmcblk1p2`，设置 `ROOTFS=rootfs-1`，否则设置 `ROOTFS=rootfs-2`；
- 根据 `/update-ok` 是否存在设置 `SURICATTA_ARGS=-c 2` 或 `-c 1`，并在存在时删除该标记；
- 从 SoC/eMMC sysfs 路径读取 CID，并构造带设备标识的 SWUpdate 参数；
- 启动参数包含 `-f /etc/swupdate/swupdate.cfg -L -e rootfs,${ROOTFS} -u ...`。

当前 cmdline 的 root 为 `/dev/mmcblk1p2`，按脚本逻辑会选择 `rootfs-1`。这是**脚本目标选择**的证据，不是 `rootfs-1` 对应物理分区的证据。

脚本存在需后续审计的健壮性问题：
- 固定字符偏移解析 cmdline 对参数顺序、前缀变化敏感；未知/解析失败时当前 `else` 会默认选择 `rootfs-2`，属于不安全的默认分支。
- `${LINE:28:14}` 与 `MMCCID=(...)` 不是可移植 POSIX `sh` 写法；必须先确认 `/bin/sh` 实际解释器及脚本启动方式。
- `-u "${SURICATTA_ARGS} ${MMCCID}"` 将拼接内容作为一个带空格的 quoted 参数传入；需通过进程 `/proc/PID/cmdline` 的 NUL 分隔内容与 SWUpdate 参数定义确认实际 argv 语义，不能仅凭 `ps` 的空格展示判断。
- 进程列表出现两个 SWUpdate PID（286、298）；需要查看 PPid、状态和 NUL 分隔 argv，确认是否为父子/工作进程，不能直接认定重复启动。

### U-Boot 环境配置

`/etc/fw_env.config` 内容为：

```text
/dev/mmcblk1  0xe0000 0x2000
```

这表示 Linux 用户空间的 fw_printenv/fw_setenv 工具被配置为从 eMMC user-area 设备 `/dev/mmcblk1` 的 byte offset `0xE0000` 读取/写入一个 `0x2000` bytes 的环境区域。换算为 offset 917,504 bytes（896 KiB）、size 8,192 bytes（8 KiB）。

该配置文件本身不能证明：
- U-Boot 编译时使用相同的环境介质、偏移和大小；
- 环境是否冗余、是否有 CRC/冗余标志布局；
- 该区域是否与原厂镜像/其他厂商数据存在约定；
- 环境变量中是否已经实现 bootcount、bootlimit 或升级回滚。

`fw_printenv` 与 `fw_setenv` 均存在；本轮尚未提供 `fw_printenv` 输出。只允许下一步执行 `fw_printenv` 只读查看，禁止执行 `fw_setenv`。

### 本轮结论

1. 已确认原厂系统使用 SWUpdate，并通过启动根设备选择 `rootfs-1` 或 `rootfs-2` 更新集合。
2. 当前运行在 p2 时脚本选择 `rootfs-1`；其物理目标尚未确认。
3. 已确认 fw_env 用户空间配置位置为 eMMC user area + 0xE0000，长度 0x2000；U-Boot 侧一致性和冗余方式未确认。
4. 现有脚本存在参数解析/传参可移植性与失败默认分支的审计点；暂不修改原厂脚本。
5. G0 仍未放行。不得执行 SWUpdate 升级、fw_setenv、分区修改或刷写。

### 下一轮只读采集

- `cat /etc/swupdate/swupdate.cfg`（只输出配置，不输出私钥；若包含凭据先脱敏）
- `cat /etc/swupdate/postupdate.sh`
- `readlink -f /bin/sh; ls -l /bin/sh`
- `fw_printenv`（只读；网络密码、密钥、token 等先脱敏）
- 对 PID 286、298 查看 `/proc/<PID>/status` 的 Name/PPid/State，以及 `od -An -tx1c /proc/<PID>/cmdline`，保留 NUL 边界
- 查看 `/proc/mtd`、`/sys/block/mmcblk1/device/name` 等只读信息；不推测 U-Boot 本体位置
