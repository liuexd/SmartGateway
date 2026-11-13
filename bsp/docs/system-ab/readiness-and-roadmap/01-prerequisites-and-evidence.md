# 当前前置条件、证据与未证实事项

状态：DESIGN / 写入 BLOCKED。整理日期：2026-11-13（Asia/Shanghai）。
适用对象：100ASK i.MX6ULL Pro；目标分支 `feature/imx6ull-system-ab-ota`。
范围：只读证据汇总和实施前置条件，不包含写板操作，不表示系统 A/B 已完成。

## 1. 证据口径与来源

本次读取远程仓库并整理用户提供的核查摘要，没有连接开发板、Ubuntu 虚拟机或原厂升级服务器，也没有重新下载、解包或计算原始镜像哈希。仓库基线为 `0e47178f8613b95d1f09a52fbe972be95bd5b35a`；下列仓库相对链接可在该提交下追溯。任何后续状态变化都应补充新的验证报告。

| 编号 | 来源 | 可支持的结论与限制 |
|---|---|---|
| E01 | [平台基线](../01-platform-baseline.md)、[eMMC 采集](../../validation/platform/G0-1-emmc-inventory.md) | 已记录的 Linux、内存、分区几何、挂载与占用；属于历史采样 |
| E02 | [U-Boot 启动链](../02-boot-chain-and-uboot.md) | 串口版本、环境、命令能力和 MMC 探测异常 |
| E03 | [p1/p3、RTC 与原厂 OTA 后续核查](../../validation/platform/G0-1B-p1-p3-rtc-followup.md) | G0-1B 至 G0-2D 的分区抽样、SWUpdate、环境和脚本证据 |
| E04 | 用户在“项目收尾与简历优化”对话提供的只读审计摘要，2026-11-13 转入本任务 | 板端工具纠正、Buildroot commit/defconfig/genimage 调用链、下载 rootfs 包差异；未附原始文件供本次复验 |
| E05 | [Supervisor S5 及 R1/R2](../../validation/ota/supervisor/D25-2-S5-service-supervisor.md)、[当前 S99smartgateway](../../../apps/service_supervisor/S99smartgateway) | 应用进程管理、init respawn、stop/start 语义和历史验收；当前代码不等于当前板端部署版本 |
| E06 | [应用 OTA S6/S7](../../validation/ota/supervisor/D25-2-S6-S7-board-upgrade-and-recovery.md) | 正常应用升级、故障回滚及跨重启中断恢复的记录；不能外推为系统槽回滚或所有掉电窗口通过 |
| E07 | [既有恢复门槛](../06-recovery-readiness-checklist.md)、[布局约束](../03-ab-layout-and-recovery-constraints.md)、[系统状态机](../05-system-ota-state-machine.md) | 设计与约束，不是已实现或已验收证据 |

E04 的原始审计报告、文件哈希明细和镜像没有随本次任务提供；本目录不虚构哈希，不将“摘要记录了差异”升级为“本次已重新比对”。后续应在合规范围内归档脱敏命令输出、文件大小和 SHA-256；不提交厂商镜像、私钥、凭据、CID 或个人目录。

## 2. 已具备的前置基础

| 项目 | 已有证据 | 可以推进的工作 | 尚不能据此声称 |
|---|---|---|---|
| 平台身份 | Linux 100ask / ARMv7、i.MX6ULL 平台记录 | 固定板型、记录构建与测试对象 | Ubuntu 主机命令输出就是板端输出 |
| 启动链 | U-Boot 2017.03、p2 的 Kernel/DTB/root 参数 | 离线审计启动策略与源码 | 已有自动选槽、bootcount 或回滚 |
| 存储 | eMMC 容量、p1/p2/p3 几何与 p2/ext4 | 计算槽位预算、识别保护范围 | p1/p3/尾部可以回收 |
| 构建入口 | Buildroot 2020.02 系列候选工程与 defconfig 链 | 主机端复现、依赖审计、裁剪实验 | 产物与板端逐字节一致 |
| 更新组件 | SWUpdate 2019.11.0、S98、Suricatta、公钥路径 | 离线分析软件集合、handlers 和事务后端 | 验签一定强制、包一定只写备用槽 |
| 应用管理 | Supervisor、应用 OTA 的实现与历史测试 | 复用 IPC、进程身份、业务探针和恢复经验 | 能修复无法启动的 Kernel/RootFS |

### 2.1 平台与运行身份

- U-Boot：`2017.03-g8ba4c5bb19-dirty`，记录的构建时间为 Jun 10 2026，Linaro GCC 6.2.1（E02）。`dirty` 表明不能仅凭 commit 后缀复现全部源码；需要补丁和完整配置。
- Linux：`4.9.88`、`armv7l`、SMP PREEMPT；构建标识中的 Ubuntu 是编译主机，不是板端发行版（E01）。
- 启动参数：`console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw`。
- `/proc/mounts`：`/dev/root / ext4 rw,relatime,data=ordered 0 0`；结合 root 参数确认当前根分区 p2（E01/E04）。
- RootFS 的 Buildroot 2020.02 是既有项目基线；候选工程的存在不证明其输出就是当前板端镜像（E01/E04）。
- MemTotal 500,948 KiB；CMA 总量 327,680 KiB（320 MiB），采样时 CmaFree 226,248 KiB。单次空闲量不能证明可缩减 CMA。
- RTC/系统时间曾为 1970-01-06，同时存在 ntpd 与 S49ntp；实际同步状态未知。后续证据使用可信主机时间、boot_id 和单调时间，不能仅依赖板端墙钟（E03）。

### 2.2 eMMC、容量与保护范围

user area：7,634,944 × 512 = **3,909,091,328 bytes**，即 3,728 MiB（约 3.64 GiB）。分区表为 DOS/MBR（E01/E04）。

| 对象 | start sector | size sector | 容量 | 类型/内容 | 当前决策 |
|---|---:|---:|---:|---|---|
| p1 | 4,098 | 2,048,000 | 1,000 MiB | MBR 0x83；blkid 未识别，file -s 为 data；512 MiB/近末尾抽样有 QML/XML 风格文本 | 用途未知，禁止写入 |
| p2 | 2,052,098 | 3,072,000 | 1,500 MiB | MBR 0x83；ext4；当前根文件系统 | 现有启动系统，必须保护 |
| p3 | 5,124,098 | 1,024,000 | 500 MiB | MBR 0x0c；blkid 未识别，file -s 为 data；六处小样本为零 | 不等于 FAT32，不等于空分区 |
| p3 后 | 6,148,098 | 1,486,846 | 约 726.97 MiB | 由几何计算的未分区尾部 | 厂商用途未知，禁止直接分配 |
| p1 前 | 0 | 4,098 | 约 2.00 MiB | 含分区表及可能的启动/环境数据 | 绝非空白预留区 |

p1/p2 的 MBR bootable 标志被记录为置位，不能据此判断 U-Boot 如何选槽。boot0/boot1 各 4 MiB，是独立硬件区域，不是 p1/p2；其 force_ro=1 不证明 U-Boot 本体位置。RPMB 不纳入本阶段布局。

板端 `blkid` 已由 E04 核实为 BusyBox 实现，帮助只支持 `blkid [BLOCKDEV]...`。旧记录里的 `blkid -p -o full` 无输出或退出码不得作为 util-linux 探测成功证据；以普通 blkid、file -s 和挂载证据为准。未识别签名既不证明为空，也不证明可以格式化。

p2 历史 df 约 677 MiB used、706 MiB available；这些是采样占用，**不是完整镜像大小**。1,500 MiB 的 raw ext4 镜像不能写入 1,000 MiB 的 p1，哪怕压缩包很小、有效文件不足 1,000 MiB。尾部约 727 MiB 与当前已用量过近，不能宣布第二完整槽可容纳。容量设计见规划 G3。

### 2.3 U-Boot 与环境持久化

已记录（E02/E03）：

- `mmcdev=1`、`mmcpart=2`、`mmcroot=/dev/mmcblk1p2 rootwait rw`。
- `loadimage/loadfdt` 使用 ext2load，从同一分区的 `/boot/zImage` 和 DTB 路径读取；脚本加载使用 `fatload mmc 1:2 ... boot.scr`。
- p2 根目录 `/boot.scr` 与 `/boot/boot.scr` 均未发现。p2 是 ext4，fatload 路径存在类型不匹配风险；当前环境的失败分支会尝试 loadimage/mmcboot，实际每一步仍需完整启动日志核对。
- help 列出 ext2load/ext4load/bootz 等；`help bootcount` 返回无此命令。这不能单独证明未编译 bootcount 支持。
- 环境筛查未发现 bootcount、bootlimit、upgrade_available、ustate 等显式变量；`update=yes` 和 updateset 只是初始化线索，不能解释为完整 A/B 状态机。
- `/etc/fw_env.config` 为 `/dev/mmcblk1 0xe0000 0x2000`：用户区偏移 917,504 bytes（896 KiB）、长度 8,192 bytes（8 KiB）。位于 p1 前区域；仅证明用户空间工具配置及已有读取结果，未证明 U-Boot 编译布局、冗余和掉电语义一致。
- U-Boot 交互式 mmc part 曾出现 no card present / mmc_init -123；显式选设备后完整只读复核记录尚待补齐。

**仍缺：** U-Boot 本体位置、匹配 dirty 构建源码、环境物理布局、冗余副本位置、bootcount 后端及 reset/power-cycle 行为。单个环境变量、命令存在或一次 fw_printenv 成功均不构成安全写入证明。

### 2.4 当前 SWUpdate 与原厂包缺口

板端版本：SWUpdate **2019.11.0**，记录的编译日期 Jul 21 2024（E01/E03）。

- S98swupdate 根据 cmdline root 选择 mode；当前 p2 分支选择 `rootfs-1`，另一分支选择 `rootfs-2`。
- `-e rootfs,rootfs-1` 表示 software set 与 mode，**不是物理分区路径**。没有对应 sw-description 不能认定 rootfs-1=p1、rootfs-2=p2。
- 当前 postupdate.sh 创建 /update-ok、sync 后重启。S98 根据标记选择 Suricatta 的 -c；板端帮助记录为 1=AGAIN、2=SUCCESS、3=FAILED，属于服务器确认，不是新系统本地健康确认。
- 帮助存在 -M/--no-transaction-marker；它不证明 marker 实际后端、设置时机、变量名或 U-Boot 消费逻辑，更不证明 bootcount 自动回滚。
- 配置有公钥路径、Suricatta、HTTP 和 nocheckcert=true；必须另证验签强制性、覆盖范围与传输认证，不能把“有公钥文件”当作完整安全链。
- /bin/sh 指向 bash；现有 substring/数组语法不能直接搬到 ash。固定字符偏移解析 root、未知 root 默认另一 mode 均应在后续新方案中消除。
- 两个相同 argv 的 SWUpdate PID 是已记录的父子关系，不能简单判断重复服务；-u 后是一个包含子参数的 argv，需按匹配版本解析实现审计。

原厂 `.swu` / `sw-description` 尚未找到：E03 的板端搜索覆盖 /tmp、/mnt、/root、/home；E04 的 rootfs.tar.bz2 检查覆盖直接归档条目，未逐层扫描嵌套压缩包。正确表述是“在已查范围未找到”，不是“原厂没有升级包”。

**两条后续路线：**

1. 原厂兼容路线：取得与板端 S98、SWUpdate 二进制和 modes 匹配的包/打包脚本，审计 images/files/scripts/handlers/bootenv 的全部写入目标。
2. 自建路线：原厂包持续不可得时，记录“不复用未知映射”的设计决策，基于已证明可用的空间、恢复链和匹配组件重新定义受信包与布局。原厂未知写入机制必须在后续受控实施中与自建 writer 隔离、统一互斥，不能同时运行两个升级入口。

第二条路线无需永远等待原厂包，但不能猜 p1/p3 用途或跳过 G0；两条路线当前均未具备板端写入条件。

### 2.5 Buildroot 候选构建链与下载包

E04 记录候选工程为 Buildroot_2020.02.x，Git commit：

`37ba456247b4f5a153049c355f77768456de54c2`

defconfig：`100ask_imx6ull-pro-aixiaozhi_defconfig`。

```text
BR2_ROOTFS_OVERLAY="board/100ask/nxp-imx6ull/rootfs-overlay/systemXiaozhi/"
BR2_ROOTFS_POST_IMAGE_SCRIPT="support/scripts/genimage.sh"
BR2_ROOTFS_POST_SCRIPT_ARGS="-c board/100ask/nxp-imx6ull/genimage-xiaozhi.cfg"
BR2_PACKAGE_SWUPDATE=y
```

已核对该脚本把 -c 参数传给 genimage --config。这证明“按此 defconfig 构建”的后处理调用链，不证明板端就是按此配置烧录。

| 配置项 | genimage-xiaozhi.cfg | 当前板端 |
|---|---|---|
| rootfs-1 / 第一分区 | 20M，无 image | p1 为 1,000 MiB，存在非零数据 |
| rootfs-2 / 第二分区 | 1500M，image=rootfs.ext4 | p2 为 1,500 MiB/ext4，当前根 |
| userdate / 第三分区 | 20M（原拼写） | p3 为 500 MiB，用途未知 |

分区名称与 SWUpdate mode 相似不构成绑定证据。即便第二分区容量吻合，整体布局仍不相同；不能用这份配置解释当前所有分区，更不能直接生成整盘镜像覆盖原盘。

下载的 rootfs.tar.bz2 含 SWUpdate、网页、启动脚本、公钥和升级后脚本，但：

| 项目 | 下载包 | 当前板端记录 |
|---|---|---|
| 启动脚本 | S97swupdate | S98swupdate |
| 启动入口 | 显式 -w Web 服务 | -u Suricatta，选择 rootfs,rootfs-1 |
| swupdate 二进制 | 摘要记录哈希不同 | 当前运行版本 |
| swupdate.cfg/postupdate.sh | 摘要记录均不同 | 板端自身版本 |

包内旧 postupdate.sh 判断 /dev/mmcblk0p2，却选择 /dev/mmcblk1p1 或 p2；按摘要中对当前 root=/dev/mmcblk1p2 的代入分析，会选中 p2。**这是旧包脚本的风险，不是当前板端 postupdate.sh 的行为结论。** 禁止直接移植该脚本，也不能把 rootfs 包当成完整恢复镜像或可直接安装的 .swu。

### 2.6 Supervisor 与应用 OTA 的真实边界

E05 的 S5-R1/R2 及当前脚本支持：

- BusyBox init respawn 拉起 Supervisor，S99 start 请求启动 Gateway。
- stop 移除 respawn 条目并停服务；start 重新注册并等待 init 拉起；失败时显式报错，没有自行 fork 的兜底。
- 历史报告记录 L12 21/21、S5-R2 27/27 板端通过。S6/S7 中“L12 未实现”的旧文字已被后续 R1/R2 证据更新，不能继续当作当前结论。
- 当前代码允许 Gateway 启动失败时打印 WARN 后结束 S99 start，因此启动脚本返回成功不能替代业务健康检查。
- 领养进程路径仍有 PID 身份检查到发信号之间的竞态边界，D 状态等覆盖限制须保留。

E06 记录 /opt/smartgateway 下 releases/current、state、packages、logs 跨重启持久，以及应用升级、北向 Mock 故障回滚、recover/ack-operation 收敛。它并未证明 /opt 已在独立 DATA 分区；现有挂载证据反而仍是根文件系统目录。迁移到共享 DATA 是未来设计。

**掉电证据口径：** E06 标题/结论写“真实断电”，但展示的命中步骤包含 sync + reboot。现有可见材料能支持受控重启中断恢复，不能独立证明未 sync 的物理断电、eMMC 缓存撕裂或所有电源窗口。G6 必须另补真实切电记录，不能继承为 PASS。

应用 ROLLED_BACK 不代表业务此刻正在运行；历史流程可能在验证后停掉测试实例。新系统确认必须独立查 Supervisor IPC、Gateway 活性与业务探针。Kernel 未启动或根挂载失败时用户态 Supervisor 根本无法运行，不能承担 bootloader 回滚职责。

## 3. 未证实事项与解除条件

| 编号 | 缺口 | 必需补充证据 | 不满足时阻断 |
|---|---|---|---|
| U01 | 精确板型、恢复工具与镜像匹配 | 硬件版本、镜像来源/长度/哈希、完整覆盖范围 | 一切启动介质修改 |
| U02 | 独立恢复入口可用 | 脱离 p2 的实际启动、备份恢复与核心功能复验记录 | 写槽、改环境、改分区 |
| U03 | 备份可恢复且一致 | user area、必要 boot 区、环境、唯一数据清单；离板双份保存与恢复核验 | 以“已复制文件”替代恢复验证 |
| U04 | p1/p3/尾部用途 | 完整只读镜像/分析、厂商布局交叉验证及保留范围 | 槽位分配 |
| U05 | 原厂模式映射 | 匹配 sw-description、打包脚本、所有 hooks/handler 目标，或明确自建路线记录 | 复用原厂升级链 |
| U06 | U-Boot 本体/环境/bootcount | 匹配源码配置、物理地址、冗余方案、冷启动/复位实测 | pending 持久化及自动切槽 |
| U07 | 构建与板端一致性 | Kernel/DTS/模块/Buildroot 配置、补丁、工具链、产物清单 | 把候选源码称为可复现板端基线 |
| U08 | 槽位容量和 ext4 兼容性 | raw 展开尺寸、文件系统特性、启动读取、增长与暂存预算 | 镜像写入 |
| U09 | 系统健康确认 | 板型必需项、期限、失败重启路径、可信确认者、回滚后业务验证 | 系统标记 confirmed |
| U10 | 持久化与掉电收敛 | 冗余状态、持久写顺序、真实断电矩阵、缓存行为记录 | 发布可靠 A/B 结论 |
| U11 | DATA 与应用共存 | 数据迁移兼容、OTA 互斥、DATA 失败时禁止确认 | 应用/系统 OTA 集成 |
| U12 | 当前部署与时间可信度 | 脱敏二进制/配置指纹、启动日志、RTC/NTP 状态 | 外推历史 PASS 为当前全量 PASS |

## 4. 风险与必须执行的保护约束

| 风险 | 约束/放行要求 |
|---|---|
| 把 mode 当设备、设备编号变化 | 固定受审布局；以设备身份、major/minor、扇区边界、运行 root 与挂载交叉核验，未知即拒绝 |
| 覆盖 p2 或与其重叠的整盘范围 | 当前禁止 p2 写入/缩容/移动；拒绝整盘写入和覆盖其 /boot；未来迁移需单独证明旧系统可恢复 |
| 原始 1500 MiB 镜像写入 1000 MiB | 按实际展开长度和文件系统声明容量检查，禁止只看压缩包或 df |
| 一份环境在断电中损坏 | 匹配 U-Boot 后端、双副本或经证明等价机制；保存 last-good；不得凭 fw_setenv 成功认定安全 |
| Linux 启动失败无法自救 | 启动前计数、加载失败分支、可信 watchdog/受控复位和超限回退必须形成闭环 |
| 新系统破坏共享数据导致旧系统也不可用 | 向后兼容/版本化迁移；确认前不做不可逆 schema 改写 |
| 原厂和自建 OTA 并发 | 全局互斥及原厂自动安装入口处置方案；本次不改变其运行状态 |
| 历史“只读挂载”触发日志回放 | 优先主机分析只读副本；如挂 ext4 副本需明确禁止日志回放；不能把普通 ro 当作无写入证明 |
| “sync 后重启”被当作任意切电保证 | 另做无预先 sync 的真实断电试验；区分受控 reboot、复位和拔电证据 |

## 5. 写入前的恢复放行门槛

以下项目全部要有记录，缺一项即不放行。它们是未来实施条件，本次提交不执行恢复或写入。

- [ ] 固定板型/存储版本，验证恢复镜像和工具的适配及哈希；明确恢复覆盖 U-Boot、环境、分区、系统和唯一数据中的哪些区域。
- [ ] 从不依赖当前 p2 的恢复介质/模式实际进入恢复环境；只有工具截图、文件下载成功或菜单可选不足以通过。
- [ ] 从停写的独立恢复环境取得一致备份；对运行中 rw p2 的直接复制不能默认视为一致镜像。记录分区表、保留区、user area 和必要 boot0/boot1 覆盖，RPMB 不盲目复制/写回。
- [ ] 备份离板保存，至少两份可校验副本，记录长度、SHA-256、读取/恢复工具和唯一数据保护方式。
- [ ] 在备用同型板/可替换介质或经单独评审的恢复试验中实际恢复并验证串口、Kernel、p2/root、ADB/网络、GPIO/I²C、Gateway/Supervisor。等价试验须明确硬件差异和剩余风险；不能先破坏唯一好系统来“验证备份”。
- [ ] 证明 p1/p3/尾部及环境区域用途、准确边界和恢复方式；没有证据不能批准回收。
- [ ] 给出仅针对已批准目标的写入范围、长度、回读、失败退出和恢复步骤；保护 p2、当前槽及 last-good，拒绝别名/重叠路径。
- [ ] 完成 G1/G0 书面验收；后续各阶段再满足各自写入门槛，不能用一次恢复通过放行所有变更。

## 6. 当前可做与下一步

现在可做：整理原始证据、寻找匹配原厂包、主机端读源码与包、复现构建、做镜像大小和依赖分析、设计恢复与故障测试。不能直接安装 .swu、写 p1/p3、重分区、在线缩 p2、保存 U-Boot 环境或替换板端启动文件。

按 [G1–G6 规划](02-g1-g6-implementation-plan.md) 推进。优先闭合恢复链、空间用途和 U-Boot 后端；同步开展主机端 G2 基线复现，避免把等待原厂包变成所有离线工作的阻塞。
