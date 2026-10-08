# SmartGateway 开发记录 / CHANGELOG

此文件记录 **GitHub main 分支已经存在的历史提交**，按开发阶段整理实现范围与里程碑。这里的“已提交”不等于该阶段所有硬件、并发或长期运行测试均已独立复验。

- 仓库：[liuexd/SmartGateway](https://github.com/liuexd/SmartGateway)
- 最后核对：2026-10-08
- 最近代码里程碑：**M7-7**；M7-8 真实硬件联调尚无完成提交
- 说明：以下是对既有 Commit 的解释，**没有改写 Git 历史**。时间按 GitHub 提交记录展示。

## M7-8 — 计划中

**真实 NODE02 WiFi 光敏节点接入和整机验收（尚未完成）**

预计工作：STM32F103 ADC 采样 → UART → ESP8266 TCP → Linux Gateway；验证 NODE01 与 NODE02 并行运行、断线重连、Server 重启、运行资源与长期稳定性。真实硬件完成情况请以未来提交及验收记录为准。

## M7-7 — 2026-10-08

**[f2dfe9c · M7-7: route CMD by node_id with reliable NACK feedback](https://github.com/liuexd/SmartGateway/commit/f2dfe9ccf0851ad0940e519afa08419947ca19ca)**

- 新增 gateway/device_manager.c/.h：设备注册与查询、互斥保护、连接所有权校验、独立命令队列。
- Server 下发 CMD 时携带目标 node_id；Gateway 通过 Device Manager 路由至 Bluetooth / WiFi Worker，避免跨线程直接操作蓝牙 fd。
- 完善路由失败和传输失败 NACK，例如 route_not_found、route_offline、route_queue_full、transport_disconnected。
- WiFi 上行增加 ACK/NACK 类型分派；Server 命令管理增加目标节点信息和应答身份校验。
- 使用非阻塞 send + poll(POLLOUT) 提供 TCP 发送超时保护。
- 新增 device_manager、command_manager、tcp_send_timeout 测试；清理误提交的二进制文件。
- 提交说明声明 make test（10/10）、TSan、ASan 和双节点路由实测通过；此处不替代独立的 CI / 硬件测试证明。

## M7-6 — 2026-09-20

**[2db89cd · M7-6: add the thread-safe message queue and server link thread](https://github.com/liuexd/SmartGateway/commit/2db89cdbde6d2fe0c23223ec55d11f94bf0e1f17)**

- 新增有界环形 message_queue：pthread_mutex、pthread_cond、push/pop/try_pop、shutdown。
- 新增 Server Link Thread，集中处理北向 TCP 上行与下行。
- Bluetooth、WiFi Worker 向公共 Queue 投递数据；主线程协调 Worker 退出及资源回收。
- 增加 Queue 单元测试。后续 M7-7 对下行 fd 所有权、发送超时和错误反馈进一步完善。

## M7-5 — 2026-09-17

**[211a824 · M7-5: move Bluetoothpath into worker thread](https://github.com/liuexd/SmartGateway/commit/211a824b1d668a9e701f7c2ef3304fd8548f9729)**

- 将蓝牙串口连接、接收、解析和断线重连逻辑迁移至独立 Bluetooth Worker。
- 形成 Bluetooth 与 WiFi Worker 并行处理的基础。

## M7-4 — 2026-09-14

**[5ef36e5 · M7-4:add WIFI client worker threads](https://github.com/liuexd/SmartGateway/commit/5ef36e5379be590734a8db948eb8513476df58cf)**

- 引入 pthread，为 WiFi Client 建立独立 Worker。
- 采用 one-connection-one-thread 模型，形成多 WiFi 客户端并发处理和生命周期管理基础。

## M7-3 — 2026-08-26

**[33eba4f · M7-3: add WiFi node TCP server path](https://github.com/liuexd/SmartGateway/commit/33eba4f26079158184eb12ff3ce8fce782673f5f)**

- Gateway 增加 WiFi TCP 监听与接收路径。
- 在原有 JSON Lines / line_parser 基础上处理 WiFi 节点上行数据。

## M7-2 — 2026-08-26

**[21a5320 · M7-2: add mock WIFI light sensor node](https://github.com/liuexd/SmartGateway/commit/21a532097ac7ada94071401d0d4c25c7dc3b7d38)**

- 新增 tools/mock_wifi_node.c。
- Linux TCP Client 模拟 NODE02，通过 JSON Lines 发送随序号变化的 LIGHT_RAW 数据。
- 为后续 WiFi Gateway 和硬件接入提供无硬件测试入口。

## M7-1 — 2026-08-25（收口）

**[035eb71 · M7-1: close NODE02 light data acceptance](https://github.com/liuexd/SmartGateway/commit/035eb71c538ab431643f77989280c85cc1712f2f)**

- 为 NODE02 的 LIGHT_RAW 增加 JSON 构建/解码 round-trip 测试。
- 补充 node_store 的 NODE02 数据更新/查询测试，使通用 KV 模型的跨节点能力得到针对性覆盖。

## M7-1 — 2026-08-19（主体）

**[b898df6 · M7-1](https://github.com/liuexd/SmartGateway/commit/b898df61008834719d154e043a7218a93dd994a1)**

- 协议与数据模型逐步由固定传感器字段转向通用 KV 字段。
- message_json 和 node_store 支持按 node_id 管理不同节点的数据。

## M5 — 2026-08-05（初始基线）

**[2b1f831 · Initial commit: complete SmartGateway M5](https://github.com/liuexd/SmartGateway/commit/2b1f8314d324ad67bc3b46e05104ccbf9c192723)**

- 单蓝牙节点网关初版，包含自定义串口帧、CRC、DATA/CMD/ACK/NACK 和 TCP/JSON 基础能力。
- 为后续 M7 的多节点与多线程扩展提供可追溯基线。

---

## 维护本文件的规则

1. 完成具有明确验收意义的阶段或功能后，追加一条记录，包含**日期、Commit 链接、变更范围、验证方式**。
2. 区分“已实现 / 已提交”和“已通过测试 / 已完成硬件验收”，避免 README 夸大当前状态。
3. 不通过 force push 重写已公开历史提交来统一命名；用此文件维护历史可读性。
4. 后续新增 Commit 的命名规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。
