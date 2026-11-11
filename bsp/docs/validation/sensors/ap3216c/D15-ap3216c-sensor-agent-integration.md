# D15：AP3216C Sensor Agent 接入与节点状态管理验收

> 验收日期：2026-10-23  
> 项目：SmartGateway-BSP / 100ASK i.MX6ULL  
> 节点：NODE04（DEV=5，AP3216C）  
> 本文是基于本地代码、开发板及 Ubuntu Server 实测日志的验收记录。**撰写时 GitHub 远端 `feature/bsp-imx6ull` 分支尚未包含本地新增的 `bsp/apps/sensor_agent/main.c` 和 D15-8 NodeStore 改动；本文档先行提交不代表对应源码已发布。**

## 1. 验收目标

D15 将 D10～D14 的 AP3216C I²C 内核驱动接入现有 SmartGateway 数据链路，完成：

1. 用户态从 `als_raw` 读取 ALS 原始数据。
2. 使用现有 DATA JSON 协议上报 NODE04（DEV=5）。
3. 周期采样、复用 TCP 长连接，并在 Gateway 断线后重连。
4. I²C 失败时不构造虚假 SAMPLE，驱动恢复后 Agent 自动继续采集。
5. 周期 HEARTBEAT 上报 Agent 活性和最近一次传感器采集状态。
6. Server NodeStore 将节点在线、传感器健康、最近有效数据分别管理；故障时当前有效值返回 NULL（显示为 N/A）。
7. 通过单元测试与端到端联调核验上述行为。

## 2. 实验环境与链路

- 开发板：100ASK i.MX6ULL 14x14；Linux 4.9.88。
- ALS 传感器：AP3216C，I²C bus 0，地址 `0x1e`，设备 `0-001e`。
- 内核模块：`sgw_ap3216`，sysfs 接口 `/sys/bus/i2c/devices/0-001e/als_raw`、`stats`。
- 板端用户态：`sensor_agent`（NODE04） → 本地 Gateway（`127.0.0.1:7000`）。
- 服务器：Gateway 转发至 Ubuntu Server（测试地址 `192.168.10.2:9000`），Server 将 DATA 交给 NodeStore。
- Agent 周期：ALS 约每 2 秒采集一次；心跳约每 10 秒发送一次；Server 节点在线超时阈值 30 秒。
- Server 的有效 ALS 查询在本轮演示中使用 30 秒有效期。**测量有效期和节点在线阈值概念上独立**。

数据链路：

```text
AP3216C ──I2C──> sgw_ap3216.ko ──sysfs als_raw──> sensor_agent (NODE04)
      ──TCP JSON Lines──> Gateway ──TCP──> Ubuntu Server ──> NodeStore
```

## 3. 协议与状态语义

### 3.1 SAMPLE

```json
{"node":"NODE04","seq":1,"type":"DATA","fields":{"DEV":"5","LIGHT_RAW":"0","KIND":"SAMPLE"}}
```

- `node=NODE04`：节点标识。
- `DEV=5`：公共协议字典中对应 AP3216C。
- `LIGHT_RAW`：驱动本次成功读取的 ALS 原始值，**0 是合法值，不是失败标志**。
- `KIND=SAMPLE`：本次产生了有效采样。

### 3.2 HEARTBEAT

```json
{"node":"NODE04","seq":2239,"type":"DATA","fields":{"DEV":"5","KIND":"HEARTBEAT","STATUS":"ALIVE","SENSOR":"ERROR"}}
```

- `STATUS=ALIVE`：Agent 能运行并构造、发送状态消息，不代表传感器必然正常。
- `SENSOR=OK/ERROR/UNKNOWN`：最近一次传感器采样状态；`ERROR` 对应读取失败，`UNKNOWN` 可用于首次采集前。
- `KIND=HEARTBEAT`：节点状态消息，**不能被当成新一次 ALS 测量**。
- SAMPLE 与 HEARTBEAT 共用同一应用层 `seq` 序号空间；失败的读取尝试不产生 SAMPLE 或新序号。
- TCP 发送函数返回成功不等于 Server 完成业务处理，重发也不等于端到端 exactly-once；本阶段未实现 Server 端 DATA ACK/去重。

**状态归属必须分离**：

| 维度 | 判断依据 | 故障示例 |
| --- | --- | --- |
| NODE ONLINE/OFFLINE | Server 最近收到该节点消息的时间 | 心跳不断，NODE04 保持 ONLINE |
| SENSOR OK/ERROR | Agent 最近一次 ALS 读取是否成功，并由心跳通告 | I²C 返回 `-ETIMEDOUT`，SENSOR=ERROR |
| LIGHT_RAW 可用/过期 | 最近有效测量、采样时间与传感器状态 | 传感器异常则当前查询 NULL（控制台 N/A），历史数据可保留 |
| 协议/通信错误 | CRC、帧解析、网络连接等独立机制 | **不能将 CRC 校验失败直接解释为 SENSOR=ERROR** |

NODE04 使用 TCP JSON Lines，上报数据本身未另加应用层 CRC 字段。已有串口帧 CRC 校验与这里的 AP3216C ALS 读取状态不是同一个故障域。

## 4. 关键实现与验收结果

### D15-1：用户态读取 sysfs — PASS

`read_als_raw()` 通过 `/sys/bus/i2c/devices/0-001e/als_raw` 获取原始值，检查打开、读取、数字解析错误。实机成功读取 `ALS RAW=0`；读失败时不把错误值写入输出参数。

### D15-2：NODE04 端到端上报 — PASS

Agent 将 `NODE04/DEV=5/LIGHT_RAW=0/KIND=SAMPLE` 编码为单行 JSON；Gateway 接收并转发，Server 成功解析并存储。第一次接入时设备类型字典尚未完善，后在 D15-3 解决。

### D15-3：设备字典与测试 — PASS

在公共帧协议中注册 `FRAME_DEVICE_AP3216C=5`，支持字符串 `"5"` 到 `AP3216C` 的映射；`tests/test_frame.c`、`tests/test_node_store.c` 相关测试通过。Server 出现 `dev=AP3216C`。

### D15-4：周期采集与 TCP 长连接 — PASS

Agent 约每 2 秒采样，重复发送 SAMPLE，正常情况下仅需一次 Gateway 建连；收到 SIGINT 可退出并关闭资源。消息使用单条 `pending_json` 缓存，完成发送前不覆盖。

### D15-5：Gateway 中断与恢复 — PASS（Agent 侧）

Gateway 中断期间观察到：

```text
[SENSOR] send failed: Broken pipe
[SENSOR] connect Gateway: Connection refused
...
[SENSOR] Gateway connected
[SENSOR] TX: ... seq=21 ...
[SENSOR] ALS=0 seq=22
```

说明 Agent 检测断链、重连，并保留尚未确认完成发送的本地 pending 消息。**该测试不证明 Server 端 exactly-once、ACK 或去重能力**。网络持续中断期间，现有单 pending 策略会阻塞新消息生成，仍有吞吐量与积压策略的改进空间。

### D15-6：I²C 故障注入与无重启恢复 — PASS

使用带 D14 统计功能的驱动版本，注入高字节读取超时：

```sh
rmmod sgw_ap3216
insmod /tmp/sgw_ap3216-d14-log.ko \
    inject_probe_failure=0 \
    inject_als_failure_stage=2

cat /sys/module/sgw_ap3216/parameters/inject_als_failure_stage
cat /sys/bus/i2c/devices/0-001e/stats
```

实际统计快照：

```text
read_success=0
read_errors=15

read_success=0
read_errors=24
```

两次统计期间累计新增 9 次失败；`dev_err_ratelimited()` 可能限制 dmesg 行数，但不减少失败计数。Agent 实测：

```text
[SENSOR] ALS read failed: Connection timed out
[SENSOR] ALS read failed: No such file or directory
[SENSOR] ALS=0 seq=1
[SENSOR] Gateway connected
[SENSOR] TX: {"node":"NODE04","seq":1,"type":"DATA","fields":{"DEV":"5","LIGHT_RAW":"0","KIND":"SAMPLE"}}
[SENSOR] ALS=0 seq=2
```

`No such file or directory` 出现在卸载驱动后 sysfs 属性暂时消失期间。恢复过程未重启 Agent；重新加载 `inject_als_failure_stage=0` 的驱动后正常采集。驱动的 `read_success/read_errors` 属于每次 probe 分配的私有数据，remove/free 后重新绑定会重置。

> 注意：`/tmp/sgw_ap3216.ko` 曾为旧版本，不含 D14 `stats` 属性。测试时须选用实际包含 `stats` 的已验证模块；模块已加载时不能直接再次 insmod 替换，须先正常 rmmod。

### D15-7：SAMPLE + HEARTBEAT 调度 — PASS（Agent 端）

在正常状态下观察到：

```text
[SENSOR] SAMPLE TX: ... seq=5 ... KIND=SAMPLE
[SENSOR] HEARTBEAT prepared seq=6 SENSOR=OK
[SENSOR] HEARTBEAT TX: ... seq=6 ... KIND=HEARTBEAT ... SENSOR=OK
[SENSOR] ALS=0 seq=7
[SENSOR] SAMPLE TX: ... seq=7 ... KIND=SAMPLE
```

故障时 Agent 仍定期发送：

```text
[SENSOR] ALS read failed: Connection timed out
[SENSOR] HEARTBEAT prepared seq=2239 SENSOR=ERROR
[SENSOR] HEARTBEAT TX: {"node":"NODE04","seq":2239,"type":"DATA","fields":{"DEV":"5","KIND":"HEARTBEAT","STATUS":"ALIVE","SENSOR":"ERROR"}}
```

采集失败期间不伪造 SAMPLE；心跳可独立报告 Agent 运行与 Sensor 错误状态。

### D15-8：NodeStore 区分在线、健康和测量有效性 — PASS（核心联调）

**改造动机**：原始 `node_store_update()` 对每条消息直接执行 `entry->data = *data`，导致无 `LIGHT_RAW` 的 HEARTBEAT 整体覆盖上一次 SAMPLE。

本地改造采用按 `KIND` 分支更新，并保存：
- `last_update`：最后收到任何消息的时间；心跳可以更新。
- `last_sample_time`：最近一次有效业务数据时间；心跳不更新。
- `sensor_status`：最近一次已知传感器状态。
- 最近一次有效业务数据：心跳不覆盖。
- 有效字段查询：检查是否已有数据、传感器状态和数据年龄；失败返回 `NULL`，注册表显示 `N/A`。

Server 实际收到：

```text
[NodeStore] node=NODE04 seq=2238 dev=AP3216C fields=DEV=5,KIND=HEARTBEAT,STATUS=ALIVE,SENSOR=OK
[NodeStore] node=NODE04 seq=2239 dev=AP3216C fields=DEV=5,KIND=HEARTBEAT,STATUS=ALIVE,SENSOR=ERROR
```

故障心跳处理**之后**的注册表实测：

```text
--- Node registry ---
  NODE04   AP3216C    [ONLINE ]  last=16:25:05
           SENSOR=ERROR LIGHT_RAW=N/A
---------------------
```

在恢复正常采样后 Server 收到：

```text
seq=2242 KIND=SAMPLE LIGHT_RAW=0
seq=2243 KIND=HEARTBEAT STATUS=ALIVE SENSOR=OK
seq=2244 KIND=SAMPLE LIGHT_RAW=0
```

此前正常状态的注册表实测：

```text
--- Node registry ---
  NODE04   AP3216C    [ONLINE ]  last=16:01:21
           SENSOR=OK LIGHT_RAW=0
---------------------
```

上述结果分别验证异常显示、采样恢复、正常状态显示；日志中没有单独提供恢复后最后一次 `nodes` 的新快照，不将其描述为已另行截取的证据。

## 5. 单元测试结果

Ubuntu 项目根目录执行：

```sh
make build/tests/test_node_store
./build/tests/test_node_store
make test
```

本地测试日志：

```text
[PASS] create/destroy test
[PASS] update/query test
[PASS] NODE02 LIGHT_RAW node_store test
[PASS] LRU eviction test
[PASS] NULL param test
[PASS] device type / online test
[PASS] foreach test
[PASS] heartbeat preserves sample

All node_store tests passed

test_crc16             PASS
test_frame             PASS
test_frame_parser      PASS
test_line_parser       PASS
test_message_json      PASS
test_message_queue     PASS
test_node_store        PASS
test_device_manager    PASS
test_command_manager   PASS
test_tcp_send_timeout  PASS
全部单元测试通过
```

注意：上表 10/10 是 `make test` 的**测试可执行程序**数量，不能误称 `tests/test_node_store.c` 有 10 个独立测试场景。关键新增用例为 `heartbeat preserves sample`，验证 HEARTBEAT(ERROR) 保留历史测量、节点继续 ONLINE、有效字段查询 NULL。

## 6. 关键复现步骤

1. 将已编译的 `sensor_agent`、含统计接口的驱动模块部署至开发板；启动 Ubuntu Server 与板端 Gateway。
2. 加载正常模式 `inject_als_failure_stage=0` 的驱动，启动 Agent；查看 Server `nodes`：预期 `ONLINE / SENSOR=OK / LIGHT_RAW=实际值`。
3. 保持 Agent 运行，正常卸载驱动并以 `inject_als_failure_stage=2` 重载；确认 Agent 输出 `Connection timed out`，而不会继续构造虚假 SAMPLE。
4. 等待至少一条 `SENSOR=ERROR` 心跳到达 Server；输入 `nodes`，预期 `ONLINE / SENSOR=ERROR / LIGHT_RAW=N/A`。
5. 不重启 Agent，将驱动以 `inject_als_failure_stage=0` 重载；等待 SAMPLE 与 OK 心跳，确认恢复正常。
6. 复跑 `make test`，检查回归结果。注意把接收日志与**心跳处理后的** NodeStore 注册表快照按时间顺序核对，避免把之前的 OK 快照误认为异常后状态。

## 7. 设计边界与待改进项

1. 软件故障注入只是模拟 I²C 返回值，不代表真实总线电气故障或硬件传感器损坏。
2. `sensor_status` 在当前 Agent 中依据最近一次 ALS 采集结果，**不代表 TCP/CRC 健康状态**；故障检测上报可能滞后一个心跳周期。
3. NodeStore 当前内存存储，Server 重启后状态丢失；节点类型信息依赖收到的 DEV 字段。
4. `last_update` 与 `last_sample_time` 不可合并；连续心跳不能使旧 SAMPLE 不断“变新”。当前使用 `time(NULL)`，系统墙上时间调整可能影响超时判断，后续可换为单调时钟。
5. 当前 NodeStore 为兼容旧设备，非 HEARTBEAT 消息普遍按最新业务数据处理；`has_sample` 命名对 GPIO EDGE 等消息不完全准确，未来宜按设备能力/消息类型进一步收紧。
6. 单 pending 缓冲意味着网络故障期间暂停新采样；暂无有界队列、丢弃统计、端到端业务 ACK/去重。
7. JSON/TCP 路径暂无独立应用层 CRC；已有 CRC16 单元测试不能替代 NODE04 全链路业务完整性保障。
8. 本轮未完成真实 I²C 物理故障、大规模压力、长时间运行、所有初始只有心跳/样本超时边界用例的实机验证。
9. GitHub 远端源码同步与本验收文档提交是两件事：须将本地已验证的 sensor_agent、NodeStore 实现和测试通过正常 Git 工作流另行提交/推送。

## 8. 验收结论

**D15 的核心功能及故障恢复链路经本地单元测试和实机日志验证通过。**

能够区分：
- **ONLINE + SENSOR=OK + LIGHT_RAW=有效值**：节点可通信且数据可用。
- **ONLINE + SENSOR=ERROR + LIGHT_RAW=N/A**：节点仍上报心跳，但当前采样无效。
- **OFFLINE**：超过节点在线超时未收到上报；不据此推断 I²C 具体故障原因。

此结论不涵盖硬件物理故障、严格端到端 exactly-once 或生产级可靠性保证。
