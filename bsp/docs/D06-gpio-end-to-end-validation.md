# D06 · GPIO 事件到 Server 端到端联调验收

> 验收日期：2026-10-15（北京时间）
>
> 开发分支：`feature/bsp-imx6ull`
>
> 关联实现提交：[80e4631](https://github.com/liuexd/SmartGateway/commit/80e4631be88f5793bccc35c09e084305c4c05f6f)
>
> **验收结论：核心端到端功能 PASS；稳定性、异常边界与性能测试 NOT TESTED。**
>
> **证据来源**：开发者提供的 i.MX6ULL 板端及 Ubuntu Server 终端原始输出。本记录基于这些可见证据，不代表 CI、独立复现或长期压力测试已经执行。部分日志为截取片段，并非完整运行日志。

## 1. 验收目标与边界

本次检查真实 GPIO 按键事件是否可沿以下路径传递，并在服务器被正确识别：

~~~text
GPIO4_IO19 (Linux GPIO 115)
  -> GPIO IRQ / 20 ms delayed-work debounce
  -> 64-entry ring buffer
  -> misc character device: /dev/sgw_event0 (poll/read)
  -> gpio_agent (NODE03, JSON Lines)
  -> ARM SmartGateway TCP listener :7000
  -> SmartGateway server_link worker
  -> Ubuntu TCP Server 192.168.10.2:9000
  -> NodeStore (GPIO_BUTTON)
~~~

**本次只验收一组真实按下 / 释放数据及心跳消息的贯通**，不把单次成功扩展为“永不丢包”“断线可靠重传”“已通过压力测试”等结论。

## 2. 环境与部署

| 项目 | 当前情况 | 证据范围 |
| --- | --- | --- |
| 开发板 | 100ASK i.MX6ULL / ARMv7 | 仓库 D01/D02 实机记录 |
| 板端 Linux | Kernel 4.9.88、Buildroot 2020.02 | 仓库 D01/D02 基线，**本次未重新检测版本** |
| GPIO | GPIO4_IO19，Linux 编号 115 | 本次使用的驱动与 Agent 源码 |
| Kernel Module | `sgw_gpio_event.ko` | 本次 `/proc/modules` 显示已加载 |
| 字符设备 | `/dev/sgw_event0` | 板端 `ls -l` 实测 |
| 板端 Gateway | `/tmp/smart_gateway`，`--no-serial` | 启动日志 |
| 板端 Agent | `/tmp/gpio_agent`（NODE03） | 发送日志 |
| Ubuntu Server | `192.168.10.2:9000` | Gateway 连接成功日志及 Server 收到的数据 |
| WiFi/节点入口 | 板端 Gateway TCP 7000 | Gateway 监听日志与 Agent 连接日志 |

> `192.168.10.2` 是**本次实验** Ubuntu Server 的 IP，不是必须固定使用的地址。`127.0.0.1` 在开发板上指开发板本机，不能用来访问其他主机的 Server。

## 3. 启动与复现步骤

以下为与本次实验一致的基本步骤。程序必须事先完成**对应 CPU 架构**的编译和部署；本次没有重新验证构建过程。

**Ubuntu 终端 A — 启动 Server：**

~~~sh
# 在 SmartGateway 仓库根目录
make server
./build/gateway_server 9000
~~~

**i.MX6ULL 终端 B — 检查驱动并启动 Gateway：**

~~~sh
ls -l /dev/sgw_event0
cat /proc/modules | grep sgw_gpio_event
cat /sys/devices/platform/sgw-gpio-event/stats

/tmp/smart_gateway --no-serial --server 192.168.10.2 --port 9000
~~~

**i.MX6ULL 终端 C — 启动 Agent：**

~~~sh
/tmp/gpio_agent
~~~

**验收操作：** 在 GPIO115 所连按键上执行一次按下、释放；对照 Agent 的 NODE03 / EDGE 上报和 Ubuntu Server 的 `[Message]`、`[NodeStore]` 日志；再次检查 sysfs 统计。

注意：`sgw_event_test` 与 `gpio_agent` 都会从同一个事件队列消费消息。验收时不要让两个消费者同时读取，否则消息可能被其中任一程序取走，不能按广播模型理解。

## 4. 实际输出与验证

### 4.1 内核模块与设备节点：PASS

板端实际执行：

~~~sh
ls -l /dev/sgw_event0
cat /proc/modules | grep sgw_gpio_event
~~~

实际输出：

~~~text
crw------- 1 root root 10, 56 Jan  6 23:16 /dev/sgw_event0
sgw_gpio_event 5107 0 - Live 0x7f016000 (O)
~~~

**判定：PASS**。字符设备已建立，模块处于 Live 状态。设备权限为 `crw-------`，当前仅 root 具备直接读写权限；若后续改用非 root 服务运行，需要另行处理权限。设备节点时间戳不用于判断验收日期。

### 4.2 GPIO 中断、队列及消费：PASS（当前样本）

**第一次统计快照（尚未启动 Agent 消费该批事件）：**

~~~text
irq_total=2
enqueued=2
dropped=0
depth=2
capacity=64
head=2
tail=0
last_sequence=2
last_level=1
last_timestamp_ns=66728540170287
~~~

**第二次统计快照（Agent 运行后）：**

~~~text
irq_total=8
enqueued=4
dropped=0
depth=0
capacity=64
head=4
tail=4
last_sequence=4
last_level=1
last_timestamp_ns=69365980216935
~~~

**判定：PASS（限定在这两次快照）**：

- GPIO 原始中断计数已增长；有事件被成功加入环形队列。
- 后一快照 `depth=0`、`head=tail=4`，结合 Agent 的事件读取/上报结果，证明该时刻队列中的已入队事件被消费。
- `dropped=0` 表示截至该快照，没有记录因队列满而丢弃的事件；**不等于网络端到端零丢包保证**。
- `irq_total=8` 与 `enqueued=4` 不相等符合“原始 IRQ 与经消抖后稳定状态变化分开计数”的设计，但**不能仅凭这组统计定量证明消抖算法效果**。

> 下文 Server 验收事件的 `EVT_SEQ=5/6` 发生在上述第二次快照之后。因为没有第三次 `stats` 原始输出，不应把第二次快照当成事件 5/6 的最终计数。

### 4.3 Agent 到 Gateway：PASS（发送接口与连接）

**此前本机地址误配置的验证**：Gateway 使用 `--server 127.0.0.1 --port 9000` 时，日志重复输出 `tcp_connect failed`，说明开发板本机未能连接该端口；之后将目标改为 Ubuntu Server 的 `192.168.10.2`。

**改正后 Gateway 的实际日志：**

~~~text
Gateway started
[BT] disabled (--no-serial)
[WIFI] listening on port 7000
[SERVER WORKER] started
[TCP] connecting to 192.168.10.2:9000...
[TCP] connected to 192.168.10.2:9000, connection = 1
~~~

**Agent 实际日志（节选）：**

~~~text
[GPIO AGENT] connected to Gateway
[GPIO AGENT] TX: {"node":"NODE03","seq":28,"type":"DATA","fields":{"DEV":"4","GPIO":"115","KIND":"EDGE","LEVEL":"0","STATE":"PRESSED","EVT_SEQ":"5","TS_NS":"69584800243295"}}
[GPIO AGENT] TX: {"node":"NODE03","seq":29,"type":"DATA","fields":{"DEV":"4","GPIO":"115","KIND":"EDGE","LEVEL":"1","STATE":"RELEASED","EVT_SEQ":"6","TS_NS":"69584990172961"}}
[GPIO AGENT] TX: {"node":"NODE03","seq":30,"type":"DATA","fields":{"DEV":"4","GPIO":"115","KIND":"HEARTBEAT","STATUS":"ALIVE","LEVEL":"1","STATE":"RELEASED"}}
~~~

**判定：PASS**。Gateway 成功监听 7000 并连接上游 Server；Agent 与 Gateway 建立连接，生成并发送按下、释放、心跳消息。单凭 Agent 的 `TX` 不能证明对端已处理，故仍以 4.4 的 Server 接收记录作为端到端证据。

### 4.4 Ubuntu Server 实际接收并由 NodeStore 处理：PASS

**开发者提供的 Ubuntu Server 终端原始输出：**

~~~text
--- Node registry ---
  NODE03   GPIO_BUTTON [ONLINE ]  last=10:41:57
---------------------
[Message] Length=157 {"node":"NODE03","seq":28,"type":"DATA","fields":{"DEV":"4","GPIO":"115","KIND":"EDGE","LEVEL":"0","STATE":"PRESSED","EVT_SEQ":"5","TS_NS":"69584800243295"}}
[NodeStore] node=NODE03 seq=28 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=EDGE,LEVEL=0,STATE=PRESSED,EVT_SEQ=5,TS_NS=69584800243295
[Message] Length=158 {"node":"NODE03","seq":29,"type":"DATA","fields":{"DEV":"4","GPIO":"115","KIND":"EDGE","LEVEL":"1","STATE":"RELEASED","EVT_SEQ":"6","TS_NS":"69584990172961"}}
[NodeStore] node=NODE03 seq=29 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=EDGE,LEVEL=1,STATE=RELEASED,EVT_SEQ=6,TS_NS=69584990172961
[Message] Length=141 {"node":"NODE03","seq":30,"type":"DATA","fields":{"DEV":"4","GPIO":"115","KIND":"HEARTBEAT","STATUS":"ALIVE","LEVEL":"1","STATE":"RELEASED"}}
[NodeStore] node=NODE03 seq=30 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=HEARTBEAT,STATUS=ALIVE,LEVEL=1,STATE=RELEASED
~~~

**判定：PASS**：

1. NODE03 已登记为 `GPIO_BUTTON`，状态为 `ONLINE`。
2. 按下与释放的 `node`、`seq`、`EVT_SEQ`、`LEVEL`、`STATE` 与 Agent 输出一致。
3. Server 不仅打印 `[Message]`，也打印 `[NodeStore]`，证明这三个样本已进入节点数据处理路径。
4. 心跳消息被处理。该单次观察不构成长期在线性测试。

**时间戳说明：** 事件 5、6 的 `TS_NS` 差值为 189,929,666 ns（约 189.93 ms）；这是两次事件采样时间戳的间隔，**不能解释为 IRQ 延迟、TCP 延迟或端到端处理耗时**。序号 5、6 在该样本中连续；不能据此断言此前所有事件都无丢失。

## 5. 验收矩阵

| 编号 | 检查项 | 结果 | 证据 |
| --- | --- | --- | --- |
| E2E-01 | GPIO 模块 Live，字符设备节点存在 | **PASS** | `/proc/modules`、`ls /dev/sgw_event0` |
| E2E-02 | GPIO IRQ 响应与事件入队 | **PASS** | `irq_total`、`enqueued` 增长 |
| E2E-03 | 队列消费与当前快照无溢出 | **PASS** | `depth=0`，`dropped=0` |
| E2E-04 | Agent 连接 Gateway 并提交 JSON Lines | **PASS** | `connected`、`TX` |
| E2E-05 | Gateway 连接 Ubuntu TCP Server | **PASS** | `connected to 192.168.10.2:9000` |
| E2E-06 | 按下事件到达 Server | **PASS** | NodeStore seq=28 |
| E2E-07 | 释放事件到达 Server | **PASS** | NodeStore seq=29 |
| E2E-08 | 心跳到达并被处理 | **PASS** | NodeStore seq=30 |
| E2E-09 | NODE03 正确显示 GPIO_BUTTON / ONLINE | **PASS** | Node registry |
| REL-01 | 空队列非阻塞 `read` 返回 `EAGAIN` 的单独实测 | **NOT TESTED** | 测试工具存在，但本次未提供执行记录 |
| REL-02 | 队列满、事件丢弃与恢复 | **NOT TESTED** | 缺少队列满实验 |
| REL-03 | 高速按键/消抖边界与按键风暴 | **NOT TESTED** | 缺少受控输入与数据统计 |
| REL-04 | 断网重连期间的丢失、重复、顺序 | **NOT TESTED** | 缺少 Server 接收侧对照 |
| REL-05 | 运行中卸载模块、进程退出、资源释放 | **NOT TESTED** | 缺少该轮验收输出 |
| REL-06 | GDB / perf 性能与故障定位 | **NOT TESTED** | 尚未开展 |

## 6. 源码结构与已知边界

关键文件：

- [内核驱动](../drivers/sgw_gpio_event/sgw_gpio_event.c)：GPIO IRQ、延迟消抖、队列、自旋锁、misc read/poll、sysfs stats。
- [GPIO Agent](../apps/gpio_agent/main.c)：事件读取、JSON Lines、心跳、非阻塞 TCP 发送与重连。
- [Gateway 主循环](../../gateway/gateway_loop.c)：`--no-serial` 运行方式下启动 WiFi/Server worker。
- [设备类型](../../common/frame.h)：`FRAME_DEVICE_GPIO_BUTTON = 4`。
- [Server NodeStore](../../server/node_store.c)：NODE03 数据处理。

当前实现的边界（不影响本次功能验收，但应留待后续）：

- 驱动在源码中固定 `SGW_GPIO=115`；通过软件注册 Platform Device，**还不是**由自定义 DTS GPIO 节点触发 OF 自动匹配和 probe。
- 内核与 Agent 各自定义 16 字节事件结构体；建议后续抽出共享 UAPI 头文件并检查 ABI。
- Agent 的 TCP `send` 成功并不意味着应用层确认；在断线时可能出现重复投递或无法证实的消息丢失，需要独立设计幂等/ACK 验收。
- `/dev/sgw_event0` 当前只对 root 开放，不意味着可直接使用非特权服务账户。
- D01 原厂镜像和系统恢复能力仍待独立验收，本次没有刷写 Kernel、U-Boot 或 DTB。

## 7. 后续验收与提交建议

后续任务按以下优先级执行：

1. **P0：固化证据**：完整保存板端 Gateway/Agent 和 Ubuntu Server 同一轮测试的终端日志；必要时补充时间、分支 SHA、构建命令、执行者与环境信息。
2. **P1：可靠性**：增加空队列 EAGAIN、队列溢出、快速按键、网络断开重连、进程退出/模块卸载的真实测试；区分“内核 dropped”与“网络丢失”。
3. **P1：Device Tree**：将 GPIO 和设备匹配信息转为设备树资源，验证 OF 自动绑定、probe/remove 与失败路径。
4. **P2：工程化**：统一 ARM 编译、共享 UAPI、重复执行的验收脚本；再使用 GDB/perf 完成定位和性能分析。
5. **P2：文档同步**：修订 `bsp/README.md` 中停留在 D03 的旧状态，并添加本验收文档导航。

## 8. 最终结论

**PASS：** 在本次真实 i.MX6ULL + Ubuntu 联调样本中，NODE03 的 GPIO 按下、释放及心跳消息，已经通过 Linux 驱动、用户态 Agent 和 SmartGateway 到达 Ubuntu Server，并在 NodeStore 中按 `GPIO_BUTTON` 正确处理。

**不包含在本次 PASS 中：** 稳定性、压力性能、端到端可靠投递语义、设备树自动绑定、系统移植/恢复验收。后续应单独补充实测证据，不能将源码具备某种机制直接等同于该机制的验收通过。
