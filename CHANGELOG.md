# SmartGateway 开发记录 / CHANGELOG

此文件记录项目的重要工程里程碑。历史提交记录用于追溯变更范围，不代表所有硬件、并发或长期运行测试均已独立复验。

- 仓库：[liuexd/SmartGateway](https://github.com/liuexd/SmartGateway)
- 当前 BSP 系统扩展分支：`feature/imx6ull-system-ab-ota`
- 当前文档体系：按 system-ab / reference / validation / archive 分类

## G2-R3C 安全审查整改与分裂崩溃恢复（待独立复审，未推送未写板）

- **确认时序与分裂崩溃恢复修复：** [sgw_bootctl.c](bsp/apps/boot_ab/sgw_bootctl.c) 与 [r3c_coordinator.c](bsp/apps/boot_ab/r3c_coordinator.c) 在任何写盘动作之前核验 `--running`、要求 `geteuid() == 0` 并屏蔽 `-c`/`--cmdline`/`--mounts`/`--run-dir`/`--sysroot`/`SGW_*` 覆盖；[r3c_txn.c](bsp/apps/boot_ab/r3c_txn.c) 新增 `r3c_txn_reconcile_confirm()`，在 `fw_env` 已确认而事务仍停留 `CONFIRMING` 的崩溃分裂现场下完成槽位摘要重验与 `CONFIRMED (consumed=1)` 收尾，随后严拒重放。
- **DATA、策略与证据时效硬化：** [r3c_data.c](bsp/apps/boot_ab/r3c_data.c) 增加 `trusted_dirs(p->device)`；[r3c_policy.c](bsp/apps/boot_ab/r3c_policy.c) 禁绝 `/` 根挂点、槽设备重名及 `>5000ms` 证据过期配置；[r3c_coordinator.c](bsp/apps/boot_ab/r3c_coordinator.c) 改为探测完成后采集 `sample_end` 并限制单轮探测耗时 `<= max_gap_ms`。
- **N09/N10 扩充复测 & G4/G1 方案准备：** [tests/test_g2r3c_n09_n10.py](tests/test_g2r3c_n09_n10.py) 扩充至 **7/7 OK**（`8.764 s`），全仓 10 组回归通过；同步整理 G4 离线接口设计 [10-g4-swupdate-and-ab-writer-design.md](bsp/docs/system-ab/10-g4-swupdate-and-ab-writer-design.md) 与 G1 板端演练清单 [G1-board-recovery-runbook.md](bsp/docs/validation/platform/G1-board-recovery-runbook.md)。

## G2-R3C — 主机端可信确认全链闭环与 N09/N10 通过（HOST-ACCEPTED，未写板）

- **完整可信确认链路：** 新增 [r3c_policy.{h,c}](bsp/apps/boot_ab/r3c_policy.c)、[r3c_txn.{h,c}](bsp/apps/boot_ab/r3c_txn.c) 与 [r3c_coordinator.{h,c}](bsp/apps/boot_ab/r3c_coordinator.c)，并将生产 [sgw_bootctl.c](bsp/apps/boot_ab/sgw_bootctl.c) `confirm` 接入统一持锁协调器（`.ota.lock` + `.sgw-transaction.lock`）：覆盖共享 DATA 槽位/保留扇区重叠校验与写入-`fsync`-读回验证、RSA/SHA-256 事务 `VERIFIED -> WRITTEN -> PENDING -> CONFIRMING -> CONFIRMED`（`consumed=1` 抗重放）、`boot_id`/`bootcount` 绑定与多轮稳定采样、及 `fw_env` 写后读回校验。
- **Gateway 机制与策略解耦：** [gateway/server_link.c](gateway/server_link.c) 与 [gateway/health_probe.c](gateway/health_probe.c) 新增工作线程原子心跳与 `SGW-LOCAL/1` 本地健康检查；通过根/属主受保护策略 `r3c-policy.conf` 解耦 `strict_forward` 与 `controlled_local` + `allow_external_degraded` 策略（默认严格 Fail-Closed，零上游垃圾流量）。
- **N09 / N10 主机端全链验收（`HOST-ACCEPTED`）：** 新增 [tests/test_g2r3c_n09_n10.py](tests/test_g2r3c_n09_n10.py)（4/4 OK）与 `r3c_data_exclusion_test`（PASS），全仓主机回归与 ARM 交叉编译全部通过（详见证据 [G2-R3C-closed-loop-host-validation.md](bsp/docs/validation/ota/G2-R3C-closed-loop-host-validation.md)）；开发板 eMMC、真实块设备挂点及 Watchdog 真机验证仍处于 `BOARD-PENDING`。

## G3-0 & G3 — Linux 4.9.88 内核与 Buildroot 2020.02 RootFS 裁剪定制（HOST-TESTED，未写板）

- **G3-0 裁剪前基线与 G3 内核/DTS 裁剪：** 新增隔离构建脚本 [bsp/kernel/build_kernel_g3.sh](bsp/kernel/build_kernel_g3.sh)、DTS CMA 裁剪补丁 [bsp/kernel/patches/0001-dts-trim-cma-from-320m-to-32m.patch](bsp/kernel/patches/0001-dts-trim-cma-from-320m-to-32m.patch)（`linux,cma` `320 MiB -> 32 MiB`，释放 `288 MiB`）及内核配置片段 [bsp/kernel/g3_trimmed.fragment](bsp/kernel/g3_trimmed.fragment)（保留全部 BSP 驱动、`ap3216@1e`、ConfigFS ADB、FEC/RTL8723BU WiFi、eMMC/ext4、WDOG1 与 `perf` 钩子）。实测 `zImage` 由 `8,873,360 B` 降至 `4,848,520 B`（`-45.4%`），`vmlinux` `text+data+bss` 降至 `9,365,578 B`（`-49.5%`），模块体积由 `3,304,232 B` 降至 `1,131,700 B`（`-65.7%`），5 个外部 `.ko` 均兼容编出。
- **G3 Buildroot 2020.02 RootFS 裁剪与 Overlay 集成：** 新增 [bsp/rootfs/g3_trimmed_defconfig](bsp/rootfs/g3_trimmed_defconfig)、[bsp/rootfs/post_build_g3.sh](bsp/rootfs/post_build_g3.sh)、[bsp/rootfs/build_rootfs_g3.sh](bsp/rootfs/build_rootfs_g3.sh)、兼容补丁 [bsp/rootfs/br_patches/](bsp/rootfs/br_patches/) 与审计清单 [bsp/rootfs/g3_package_audit.md](bsp/rootfs/g3_package_audit.md)。剔除 Qt5/LLVM/Mesa/LVGL/PulseAudio/ALSA/BlueZ5/`eudev hwdb.bin` 等冗余组件，`defconfig` 包从 `136` 精简至 `35`（解析包 `315 -> 152`），`rootfs.ext4` 规格 `128 MiB`（`-O ^metadata_csum`，实际文件 payload `31,401,349 B`），集成 `adbd`、`swupdate`、冗余 `fw_printenv`、`sgw_bootctl`/`sgw_watchdog_guard`/`sgw_system_health`、`service_supervisor`、`smart_gateway`、`ota_agent` 与 `gdbserver`。
- **主机回归与边界：** 新增 [tests/test_g3_kernel_rootfs.py](tests/test_g3_kernel_rootfs.py)（`2/2 OK`），写入 [09-g3-kernel-and-rootfs-trimming.md](bsp/docs/system-ab/09-g3-kernel-and-rootfs-trimming.md) 与 [G3-kernel-and-rootfs-trimming.md](bsp/docs/validation/platform/G3-kernel-and-rootfs-trimming.md)；上板启动耗时与真实 `/proc/meminfo` 标记为 `NOT RUN / PENDING BOARD`。

## G1 & G2 P0/P1 安全加固与证据边界对齐（HOST-TESTED，未写板）

- **G1 备份保护加固（P0）：** `g1_emmc_backup_verify.sh` 在创建目录前检查目标所在文件系统，修复板端 `df` 返回 `/dev/root` 时可能漏判 eMMC 根挂载的问题，默认 Fail-Closed 拒绝向未证明离板的 `/dev/root` 写入；新增 `tests/test_g1_backup_guard.py`。
- **G2 Fail-Closed 健康确认与 Watchdog 守护（P0）：** 移除 `sgw_bootctl confirm` 的 `health_ok=1` 默认值，新增 `ab_health.c` 校验 `/proc/cmdline` + `/proc/mounts` 运行槽位、系统文件及 `service_supervisor` 实时 IPC 存活 PID；新增 `sgw_watchdog_guard` 接管 `/dev/watchdog` 并验证超时停喂不写 `'V'` 魔术字复位路径。
- **单份环境迁移与编号统一（P1 / 文档）：** 新增 `migrate_single_to_redund_env.sh` 与单区->双区迁移实测；统一 `02-g1-g6-implementation-plan.md` 与当前阶段编号，明确 G3 裁剪前须先完成裁剪前基线构建（G3-0），并单独列出板端真机待补齐项（外部 SD 启动备份、真机 U-Boot `bootcmd`、`p1` 启动与 `WDOG1` 冷断电串口日志）。

## G1 & G2 — U-Boot A/B 启动管理与主机端验收（HOST-TESTED，未写板）

- **G1 恢复基线与工程准备（HOST-VERIFIED）：**
  - 在 `feature/imx6ull-system-ab-ota` 分支建立只读核查脚本 [bsp/tools/g1_check_host_env.sh](bsp/tools/g1_check_host_env.sh) 与防自毁只读备份校验脚本 [bsp/tools/g1_emmc_backup_verify.sh](bsp/tools/g1_emmc_backup_verify.sh)。
  - 完整记录 `Uboot-2017.03`、`Linux-4.9.88`、`Buildroot_2020.02.x`、Linaro GCC 6.2.1 与 Buildroot GCC 7.5.0 工具链基线；不触碰开发板 eMMC 与现网 `p2`。
- **G2 U-Boot A/B 启动与 Watchdog（HOST-TESTED）：**
  - 完成 U-Boot 2017.03 审计：确认 `CONFIG_BOOTCOUNT_ENV` + `CONFIG_ENV_OFFSET_REDUND`（`0xE0000` + `0xE2000`）是当前硬件唯一支持冷断电持久化的计数后端。
  - 新增隔离补丁与构建脚本 [bsp/uboot/patches/0001-mx6ullevk-ab-bootcount-redund-env-watchdog.patch](bsp/uboot/patches/0001-mx6ullevk-ab-bootcount-redund-env-watchdog.patch)、[bsp/uboot/build_uboot_ab.sh](bsp/uboot/build_uboot_ab.sh)，实现 `sgw_*` 状态机、`bootlimit=3`、`altbootcmd` 自动回滚、`panic=5` 与 `WDOG1` 60s 硬件看门狗。
  - 新增 `sgw_bootctl` 工具与 C/Python 主机测试套件（隔离 `update`/`recovery_status`），全部通过主机测试并保持可交叉编译为 ARMv7 二进制；未经板端真机验证的功能均显式标为主机验证状态。

## 2026-11-11 — BSP 文档结构整理

**文档体系从阶段编号平铺调整为按工程模块分类。**

- 更新根目录 README 和 `bsp/README.md`，反映当前 i.MX6ULL BSP、应用级 OTA 与系统级 A/B OTA 的真实边界。
- 将 BSP 设计、参考、验收和历史资料分为 `system-ab/`、`reference/`、`validation/`、`archive/`。
- 删除过时的 D16 后 42 天路线图，不再维护旧 D26–D34 RS485/Modbus 计划。
- 保留 GPIO、AP3216C、GDB/Perf、Supervisor 和应用级 OTA 历史验收报告。
- 新增系统级 OTA 恢复就绪检查表；当前仍未达到分区修改和系统刷写放行条件。
- 后续文档目录、命名、验收状态和提交规则见 [CONTRIBUTING.md](CONTRIBUTING.md)。

本次为文档维护，不改变运行时代码、驱动行为、OTA 实现或 eMMC 内容。

## M7-8 — 计划状态以实际硬件验收为准

真实 NODE02 WiFi 光敏节点接入和整机验收不能仅凭历史计划认定完成。当前项目主线已扩展到 i.MX6ULL BSP 和系统级 OTA；旧 M7 计划仅作为历史背景，不再作为当前 BSP 任务排期。

## 历史 M7 / M5 里程碑

以下记录保留原始开发历史。这里的提交与测试描述以当时记录为准，不替代当前分支复验。

### M7-7 — 2026-10-08

**[f2dfe9c · route CMD by node_id with reliable NACK feedback](https://github.com/liuexd/SmartGateway/commit/f2dfe9ccf0851ad0940e519afa08419947ca19ca)**

- 新增 Gateway Device Manager：设备注册与查询、互斥保护、连接所有权校验和独立命令队列。
- Server CMD 按 node_id 路由至 Bluetooth / WiFi Worker。
- 完善 route_not_found、route_offline、route_queue_full、transport_disconnected 等 NACK。
- WiFi 上行 ACK/NACK 分派和 Server 命令身份校验。
- TCP 发送超时保护及相关单元测试。

### M7-6 — 2026-09-20

**[2db89cd · thread-safe message queue and server link thread](https://github.com/liuexd/SmartGateway/commit/2db89cdbde6d2fe0c23223ec55d11f94bf0e1f17)**

- 新增有界线程安全 message_queue。
- 新增 Server Link Thread，集中处理北向 TCP 上行与下行。
- Bluetooth、WiFi Worker 向公共 Queue 投递数据。

### M7-5 — 2026-09-17

**[211a824 · move Bluetooth path into worker thread](https://github.com/liuexd/SmartGateway/commit/211a824b1d668a9e701f7c2ef3304fd8548f9729)**

- 蓝牙串口连接、接收、解析和断线重连迁移至独立 Worker。

### M7-4 — 2026-09-14

**[5ef36e5 · add WiFi client worker threads](https://github.com/liuexd/SmartGateway/commit/5ef36e5379be590734a8db948eb8513476df58cf)**

- 引入 pthread WiFi Client Worker。

### M7-3 — 2026-08-26

**[33eba4f · add WiFi node TCP server path](https://github.com/liuexd/SmartGateway/commit/33eba4f26079158184eb12ff3ce8fce782673f5f)**

- Gateway 增加 WiFi TCP 监听与接收路径。

### M7-2 — 2026-08-26

**[21a5320 · add mock WiFi light sensor node](https://github.com/liuexd/SmartGateway/commit/21a532097ac7ada94071401d0d4c25c7dc3b7d38)**

- 新增 Mock WiFi 节点，用于 JSON Lines 数据链路测试。

### M7-1 — 2026-08-19 至 2026-08-25

- [035eb71 · close NODE02 light data acceptance](https://github.com/liuexd/SmartGateway/commit/035eb71c538ab431643f77989280c85cc1712f2f)
- [b898df6 · M7-1](https://github.com/liuexd/SmartGateway/commit/b898df61008834719d154e043a7218a93dd994a1)

### M5 — 2026-08-05

**[2b1f831 · Initial commit: complete SmartGateway M5](https://github.com/liuexd/SmartGateway/commit/2b1f8314d324ad67bc3b46e05104ccbf9c192723)**

- 单蓝牙节点网关初版，包含自定义串口帧、CRC、DATA/CMD/ACK/NACK 和 TCP/JSON 基础能力。

## 维护规则

1. 完成具有明确验收意义的功能或工程里程碑后追加记录，包含日期、Commit、变更范围和验证方式。
2. 区分已实现、已提交、主机测试通过和板端验收通过。
3. 不通过 force push 改写公开历史；通过 CHANGELOG 补充背景。
4. 文档目录与验收规则见 [CONTRIBUTING.md](CONTRIBUTING.md)。
