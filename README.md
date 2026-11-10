# SmartGateway

**基于 C / Embedded Linux / STM32 的蓝牙 + WiFi 异构多节点智能网关**

> A multi-node IoT gateway built with C, POSIX threads and TCP sockets, bridging Bluetooth RFCOMM and WiFi devices to a unified TCP/JSON server.

**开发状态：M7-7 已提交，M7-8 真实 WiFi 光敏节点联调待完成。** 本仓库当前包含网关、上位 TCP Server、Linux Mock 节点和测试代码；不将尚未提交的 MCU 固件或未完成的硬件验收描述为已交付功能。

[项目演进与历史 Commit](CHANGELOG.md) · [提交规范与开发流程](CONTRIBUTING.md) · [查看全部 Commits](https://github.com/liuexd/SmartGateway/commits/main)

## 项目简介

SmartGateway 是一个从单蓝牙节点逐步扩展到多节点异构接入的嵌入式 Linux / Linux C 项目。项目重点不是单纯连接传感器，而是围绕 **TCP 网络通信、串口协议、pthread 并发模型、线程安全队列、设备生命周期管理和 CMD/ACK/NACK 闭环** 构建可维护的网关。

- **NODE01（蓝牙）**：STM32F103 + HC-06，温湿度数据上报，LED 命令与 ACK/NACK 反馈。
- **NODE02（WiFi）**：当前提供 Linux Mock WiFi 光敏节点，通过 TCP 发送 LIGHT_RAW；规划中的真实方案为 STM32F103 + ESP8266 + 光敏传感器（M7-8）。
- **Linux Gateway**：南向连接设备，北向通过 JSON Lines 对接 TCP Server。
- **TCP Server**：维护节点数据，处理命令下发、ACK/NACK 匹配、超时和重试。

## 系统架构

~~~mermaid
flowchart TB
    N1["NODE01 · STM32 + HC-06<br/>温湿度 / LED"]
    N2["NODE02 · Linux Mock WiFi<br/>真实 STM32 + ESP8266 待 M7-8"]
    BT["Bluetooth Worker<br/>RFCOMM / serial"]
    WA["WiFi Listen / Accept<br/>TCP :7000"]
    WW["WiFi Client Workers<br/>one connection, one thread"]
    Q["Thread-safe Upstream Queue"]
    DM["Device Manager<br/>node_id → transport / owner / command queue"]
    SL["Server Link Thread<br/>Northbound TCP"]
    SV["TCP Server :9000<br/>Node Store / CMD / ACK / NACK"]

    N1 <-->|Bluetooth| BT
    N2 <-->|WiFi TCP| WA
    WA --> WW
    BT --> Q
    WW --> Q
    Q --> SL
    SL <-->|JSON Lines| SV
    SL -->|route CMD| DM
    DM -->|per-node command| BT
    DM -->|per-node command| WW
~~~

**设计原则：** 上行由设备 Worker 生产消息、Server Link Thread 消费；下行通过 Device Manager 按 node_id 路由，设备 fd 由对应 Worker 负责发送，避免多个线程直接竞争同一通信资源。

## 主要能力

| 模块 | 已实现的功能 |
| --- | --- |
| 协议处理 | CRC16-CCITT-FALSE、DATA/CMD/ACK/NACK、KV 字段、JSON Lines 半包/粘包处理 |
| 蓝牙接入 | RFCOMM 串口读写、独立 Bluetooth Worker、断线重连 |
| WiFi 接入 | TCP 监听、多 Client Worker、NODE02 Mock 数据上传 |
| 并发通信 | pthread、mutex、condition variable、有界环形消息队列、Server Link Thread |
| 设备管理 | 设备注册、在线状态、最近活跃时间、连接所有权校验、设备专属命令队列 |
| 命令闭环 | 按 node_id 路由、ACK/NACK、失败反馈、命令重试与超时 |
| 网络可靠性 | TCP 重连、非阻塞发送、poll 可写等待及发送超时 |
| 测试 | 协议/队列/设备管理/命令管理/TCP 超时单测，以及独立集成测试目标 |

上述“已实现”指代码能力。**多节点长期稳定性、异常网络、真实硬件集成等仍需完整验收**，不代表已通过全部可靠性测试。

## 协议示例

蓝牙串口链路使用带 CRC16 的自定义帧，逻辑格式为：

~~~text
@数据区*CRC\r\n
~~~

Gateway 与 Server 使用 **JSON Lines**：一条 JSON 消息占一行，以换行符结尾。业务字段使用通用 fields 对象，字段值当前按字符串处理。

~~~json
{"node":"NODE01","seq":101,"type":"DATA","fields":{"T":"253","H":"601"}}
{"node":"NODE02","seq":24,"type":"DATA","fields":{"LIGHT_RAW":"2841"}}
{"node":"NODE01","sender":"SERVER","seq":3001,"type":"CMD","fields":{"LED":"1"}}
{"node":"NODE01","seq":3001,"type":"ACK","fields":{"LED":"1"}}
{"node":"NODE01","seq":3001,"type":"NACK","error":"bad_cmd"}
~~~

- T、H：温度和湿度数值扩大 10 倍保存，如 253 表示 25.3 ℃。
- LIGHT_RAW：ADC 原始采样值，**不是 lux 照度值**。
- CMD 中的 node 是路由目标；具体字段以 common/message_json.c 的实际编码为准。

## 目录结构

~~~text
SmartGateway/
├── common/              # CRC16、帧协议、行解析、JSON 编解码
├── gateway/             # Linux Gateway、Bluetooth/WiFi workers、
│                        # message_queue、server_link、device_manager
├── server/              # TCP Server、node_store、command_manager
├── tools/               # 蓝牙 Mock Node、WiFi Mock Node
├── tests/               # 单元测试与联调工具
├── README.md            # 项目介绍、运行方法
├── CHANGELOG.md         # 各阶段与历史提交记录
├── CONTRIBUTING.md      # Git Commit 规范、开发与验收流程
└── makefile
~~~

## 构建与快速体验（Linux）

### 依赖

Linux、GCC、GNU Make 和 pthread；使用模拟串口时需要 socat。部分 TCP 集成测试依赖 nc（netcat）与 timeout。嵌入式板端部署需要与目标平台适配的工具链及环境。

~~~bash
git clone https://github.com/liuexd/SmartGateway.git
cd SmartGateway
make clean
make all
~~~

构建生成：

- build/smart_gateway — Linux Gateway
- build/gateway_server — TCP Server
- build/mock_node — 蓝牙链路的模拟串口节点
- build/mock_wifi_node — WiFi TCP 光敏模拟节点

### Linux Mock 联调

使用独立终端，保持各进程运行。下面示例采用本机回环地址，不需要真实蓝牙或 ESP8266 硬件。

**终端 A — 建立虚拟串口对**

~~~bash
socat -d -d pty,raw,echo=0,link=/tmp/ttyGW pty,raw,echo=0,link=/tmp/ttyNODE
~~~

**终端 B — 启动 TCP Server（默认示例端口 9000）**

~~~bash
./build/gateway_server 9000
~~~

**终端 C — 启动 Linux Gateway（WiFi 监听端口 7000）**

~~~bash
./build/smart_gateway --serial /tmp/ttyGW --baud 9600 --server 127.0.0.1 --port 9000
~~~

**终端 D — 启动蓝牙 Mock 节点**

~~~bash
./build/mock_node /tmp/ttyNODE
~~~

**终端 E — 启动 WiFi Mock 节点**

~~~bash
./build/mock_wifi_node 127.0.0.1 7000 NODE02
~~~

运行后可在 Server 控制台尝试：

~~~text
nodes
led NODE01 1
led NODE01 0
help
~~~

当前仓库的 mock_wifi_node 以约 2 秒间隔发送 LIGHT_RAW 数据，**仅模拟上行数据**；不能将其视为已完成的 NODE02 真实 WiFi 下行执行器或硬件 ACK/NACK 测试。

## 测试

~~~bash
make test            # 10 个独立单元测试程序
make test-tcp        # 需要 nc 等依赖的 TCP 集成测试
make tests-bin       # 构建单测、集成测试及手动联调工具
make test-asan       # AddressSanitizer + UBSan
make test-tsan       # ThreadSanitizer

# OTA 与服务管理（D23 / D25-2）
sh tests/setup_ota_test_env.sh                     # 一次性准备 OTA 测试产物
python3 tests/test_ota_northbound_fault.py --prepare   # 北向故障与回滚
python3 tests/test_s3_preflight_running.py             # 前置校验完整性
python3 tests/test_s3r4_recovery.py                    # 激活前失败恢复
python3 tests/test_s4_fault_matrix.py                  # 故障矩阵 F1–F10
python3 tests/test_s5_supervisor_e2e.py                # 服务监督进程

make supervisor        # 构建 Service Supervisor
make supervisor-test   # IPC 协议 + supervisor 单元测试
make supervisor-arm    # ARM 交叉编译
~~~

M7-7 提交说明记录了单测与 Sanitizer 通过，但 GitHub 仓库目前未配置可供公开查阅的 CI 工作流运行记录。实际测试结果以当前机器与环境中的运行输出为准；部分 Sanitizer 对系统环境有要求。

## 开发里程碑

| 阶段 | 内容 | 状态 |
| --- | --- | --- |
| M5 | 单节点蓝牙智能网关基础版 | 历史基线提交 |
| M7-1 | 通用 KV 数据与 NODE02 LIGHT_RAW 测试 | 已提交 |
| M7-2 | Linux Mock WiFi 光敏节点 | 已提交 |
| M7-3 | Gateway WiFi TCP Server | 已提交 |
| M7-4 | WiFi 多连接 pthread Worker | 已提交 |
| M7-5 | Bluetooth Worker 线程化 | 已提交 |
| M7-6 | 线程安全消息队列与 Server Link Thread | 已提交 |
| M7-7 | Device Manager、目标节点路由、错误反馈 | 已提交 |
| M7-8 | 真实 STM32 + ESP8266 光敏节点、双节点最终联调 | **计划中 / 未验收** |

完整 Commit、提交时间和每阶段关键变化请查看 [CHANGELOG.md](CHANGELOG.md)。M7-8 完成后还需要回归测试、断线重连与长期稳定性验收，才能正式结项。

## BSP 与 OTA 里程碑（i.MX6ULL）

板端：100ASK i.MX6ULL Pro / Linux 4.9.88 / Buildroot 2020.02 / glibc 2.30 / BusyBox init。

| 阶段 | 内容 | 验证环境 | 状态 |
| --- | --- | --- | --- |
| D25-1 | ARM 交叉编译与板端兼容性 | 板端 | 通过 |
| D25-2 S1–S2 | 进程身份（pidfile + starttime + boot_id） | x86 | 通过 |
| D25-2 S3 | 升级流程插入前置检查 | x86 | 通过 |
| D25-2 S3-R~R4 | 安全复审修正、前置校验完整性、激活前恢复 | x86 | 通过 |
| D25-2 S4 | 故障注入矩阵 F1–F10 | x86 | 通过（67 项） |
| D25-2 S5 | Service Supervisor + BusyBox init | **x86 + 板端** | 通过 |
| D25-2 S6 | 板端正常升级与故障回滚 | **板端** | 通过 |
| D25-2 S7 | 板端掉电恢复 | **板端（真实断电）** | 通过 |

### OTA 与服务管理解决的问题

| 问题 | 原状 | 现状 |
| --- | --- | --- |
| L3 前置校验依赖 pidfile | Gateway 由别的方式启动时 pidfile 不存在，OTA 会启动第二个实例 | **已解决**：所有权在 supervisor 内存中，板端实测「删掉 pidfile 仍正确」 |
| L6 停止外部进程的 TOCTOU | pidfd 需 Linux 5.1+，板端 4.9.88 无法消除 | **已解决**：supervisor 停止自己的子进程，pid 不可能被复用 |

两者通过**转移所有权**解决，而非更仔细的检查。

部署结构：

```
/opt/smartgateway/
├── bin/{ota_agent,service_supervisor}
├── current -> releases/v0.x.0
├── releases/v0.x.0/{smart_gateway,manifest.json}
├── run/          supervisor.sock, service.pid
├── state/ota_state.json
└── service.conf
/etc/init.d/S99smartgateway
```

### 已知限制（不报告为 PASS）

| 编号 | 内容 |
| --- | --- |
| L10 | SIGKILL 也无效（D 状态）分支无集成测试 —— x86 与板端均无法构造 |
| L12 | supervisor 崩溃自愈（G4b）未实现，需 `/etc/inittab` 的 `respawn`，与现有 `start-stop-daemon` pidfile 管理冲突 |
| — | 串口模式：全部验证以 `--no-serial` 运行，真实 `/dev/ttymxc2` 路径未板端验证 |
| — | 真实北向云平台、eMMC 坏块/只读挂载未做故障注入 |
| L5 | 无 CI，所有 PASS 均为手工执行 |

详细设计见 `bsp/docs/D25-2-*.md`。

## 后续方向（非 M7 必选）

可根据需要扩展 epoll / thread pool、MQTT、TLS、systemd、SQLite、配置管理和结构化日志。这些能力属于后续规划，不属于当前已交付功能。

## 项目定位

面向嵌入式 Linux / Linux C 开发实践，系统性展示 Socket、串口、网络协议解析、pthread 同步与并发资源生命周期管理。欢迎通过 Issues 交流具体问题；提交规范见 [CONTRIBUTING.md](CONTRIBUTING.md)。
