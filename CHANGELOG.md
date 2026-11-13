# SmartGateway 开发记录 / CHANGELOG

此文件记录项目的重要工程里程碑。历史提交记录用于追溯变更范围，不代表所有硬件、并发或长期运行测试均已独立复验。

- 仓库：[liuexd/SmartGateway](https://github.com/liuexd/SmartGateway)
- 当前 BSP 系统扩展分支：`feature/imx6ull-system-ab-ota`
- 当前文档体系：按 system-ab / reference / validation / archive 分类

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
