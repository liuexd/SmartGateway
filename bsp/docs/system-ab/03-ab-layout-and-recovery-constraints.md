# A/B Storage Layout and Recovery Constraints

## 1. 目标

为 i.MX6ULL 设计 Kernel + DTB + RootFS 完整双槽升级，保留独立 DATA 分区，且在新系统通过健康确认前不破坏最后一个已确认系统。本文件只定义约束，不提供可直接执行的分区写入命令。

## 2. 当前容量事实

eMMC user area 共 7,634,944 个 512-byte sectors（约 3,728 MiB）。

现有分区：
- p1：start 4,098，size 2,048,000 sectors（1,000 MiB），用途未知；
- p2：start 2,052,098，size 3,072,000 sectors（1,500 MiB），当前 ext4 RootFS；
- p3：start 5,124,098，size 1,024,000 sectors（500 MiB），用途未知。

p3 后按扇区算术剩余 1,486,846 sectors，约 726.97 MiB。p1 前另有约 2 MiB 未包含在分区中。

当前 p2 的 ext4 使用量约 677 MiB，可用约 706 MiB。df 数值是当前文件系统使用情况，不等于压缩镜像大小，也不等于 A/B 槽位最终需求。

## 3. 容量初步判断

若保持 p1、p2、p3 不变，仅使用尾部约 727 MiB：
- 不足以容纳一个与当前 p2 同等 1,500 MiB 的完整 RootFS 槽位；
- 也不能据此断定精简 RootFS 后一定可行，因为还要容纳 Kernel、DTB、文件系统元数据、升级临时空间和增长余量；
- 当前 p2 已使用约 677 MiB，复制完整系统仅当前已用数据就接近尾部空间，尚未计算保留空间和镜像增长。

因此尾部空间可作为受控实验或小型独立分区候选，但不能直接宣布足以实现完整 A/B。

## 4. 分区布局设计原则

目标逻辑组成：
- U-Boot：暂时保持现有单份，不纳入 OTA。
- SLOT_A：Kernel_A + DTB_A + RootFS_A。
- SLOT_B：Kernel_B + DTB_B + RootFS_B。
- DATA：跨槽位持久化的应用数据、配置、日志和 OTA 状态。
- Boot Control Metadata：记录 active、pending、last-good、尝试次数、版本、序号和校验信息。

Kernel/DTB 存放方式待决策：
- 方案一：每个 RootFS 分区内放独立 /boot/zImage 与 DTB，由 U-Boot 从对应 ext4 分区加载；
- 方案二：独立 BOOT_A、BOOT_B 分区，再分别挂载 ROOTFS_A、ROOTFS_B。

当前 U-Boot printenv 指向 mmc 1:2，使用 ext2load 读取 /boot/zImage 和 DTB；help 中也列出 ext4load。方案一可能减少分区数量，但必须实测 U-Boot 对 ext4 分区和文件的读取能力。

## 5. DATA 分区要求

DATA 应保存跨系统升级必须保留的状态，例如：
- SmartGateway releases/current；
- 应用 OTA 状态和操作日志；
- System OTA 包暂存区与状态；
- 网关配置；
- Supervisor 持久化状态；
- 业务日志和必要数据。

新系统启动时必须先挂载 DATA，再启动依赖它的 Supervisor、Gateway 和 Agent。

系统 A/B 与应用级 OTA 分离：
- 应用 OTA 仅更新 DATA 中的应用 release；
- 系统 OTA 仅写入非活动系统槽；
- 两者共用互斥锁或统一升级协调机制，防止并发升级和状态冲突。

DATA 文件系统、容量、备份和掉电一致性需单独设计。日志应设置容量限制和轮转。

## 6. Boot Control Metadata 最低要求

逻辑字段建议：

```text
format_version
sequence_number
active_slot
pending_slot
last_good_slot
upgrade_available
boot_attempts
boot_limit
slot_a_version
slot_b_version
boot_successful
crc
```

元数据至少需要两份独立记录或等价的断电安全机制：
- 每条记录带递增 sequence_number；
- CRC 检查结构完整性；
- 更新时先写非当前有效副本；
- 完整写入并回读验证后，才使新记录成为最新有效记录；
- 启动时选择 CRC 正确且序号最新的记录；
- 明确底层写入原子性、缓存刷新和 fsync/flush 语义。

不能简单将普通文件 rename 或单个 U-Boot 环境变量视为天然原子且断电安全。

## 7. 恢复门槛 G0

以下事项全部完成前，不修改 eMMC 分区表、不写块设备、不执行 saveenv/fw_setenv、不覆盖 U-Boot/Kernel/DTB/RootFS：

1. 原厂镜像和烧写工具与实际板型、存储版本匹配；
2. 能够从不依赖当前 RootFS 的介质进入恢复模式；
3. 已备份 eMMC user area 及必要的 boot0/boot1 内容（先评估读取风险和方式）；
4. 已明确 U-Boot 本体和环境变量存储位置；
5. 已确认恢复操作不会误覆盖板卡序列号、校准数据或其他唯一数据；
6. 已在安全条件下演练恢复流程；
7. 恢复后验证串口启动、Kernel、RootFS、网络及原有 Gateway 功能。

## 8. 下一步决策所需证据

- p1、p3 的分区表类型、文件系统、标签和内容；
- eMMC 原始分区表只读导出；
- p1、p3 是否含启动文件、厂商恢复数据或其他保留数据；
- p2 目录空间分布及精简 RootFS 后预计占用；
- U-Boot 源码配置中的环境存储位置、大小和冗余方式；
- U-Boot 在显式 mmc dev 1、mmc rescan 后能否读取分区；
- 原厂恢复工具与完整镜像的可用性。

## 9. 当前结论

- 当前 p2 是运行中的 ext4 根文件系统。
- p1 和 p3 不能在用途未知时分配给 A/B。
- 尾部约 727 MiB 是算术推算出的候选未分配空间，不等于已确认可安全使用。
- 仅利用尾部空间建立完整第二系统槽位的容量风险较高。
- 完整 A/B 很可能需要重新规划现有 user area 分区，但必须先核实 p1/p3 和恢复能力。
- 不更新 U-Boot 本体是本阶段范围约束，不代表 U-Boot 环境区没有单点风险。
