# D23-6-8 北向 TCP 故障与 OTA 自动回滚验收

> 工程：SmartGateway-BSP
> 测试对象：`bsp/apps/ota_agent`（x86-64 集成测试构建 `ota_agent_x86`）
> 故障注入环境：`build/ota-test/install-x86-fault.2e0Csr`
> 验收日期：2026-11-05
> 结论：**PASS**（28/28 项通过）

---

## 1. 测试目标

验证新版 Gateway 正常启动、L1（南向 TCP 7000 就绪）检查通过，但**北向与 Mock Server 的 TCP 通信真实失败**时，OTA 能通过业务健康检查（L2）识别异常，自动回滚并恢复旧版 Gateway 的业务功能。

关键约束：

- 故障必须由**真实系统调用**触发，不得用 `OTA_TEST_FORCE_L2_FAIL` 直接短路返回失败；
- 不得修改已通过正常升级验收的 `install-x86.9L0EFT` 环境。

---

## 2. 测试环境

| 项目 | 值 |
| --- | --- |
| 安装目录 | `build/ota-test/install-x86-fault.2e0Csr` |
| OTA 工具 | `bsp/apps/ota_agent/ota_agent_x86` |
| 旧版本 | `0.1.0-test1` |
| 候选版本 | `0.1.1-test1` |
| 候选包 | `build/ota-test/real-x86/v0.1.1-test1` |
| 初始状态 | `CONFIRMED` |
| 初始 `current` | `releases/v0.1.0-test1` |
| 测试协调器 | `tests/test_ota_northbound_fault.py` |
| 运行日志 | `build/ota-test/d23-6-8-northbound-fault.log` |

### 初始快照（测试前）

```
State     : CONFIRMED
Confirmed : 0.1.0-test1
Current   : releases/v0.1.0-test1

releases/v0.1.0-test1/smart_gateway
  4970cb84671b5ab788095b9ee2bb953614d5838178812658b879e0642c33548a   86184 B
releases/v0.1.1-test1/smart_gateway
  64a39294e359bff66a5f46de29fdca77bcd6e7777060050147f28286e25c6e44   86184 B
```

---

## 3. 故障注入方式

### 3.1 为什么不能沿用 `OTA_TEST_FORCE_L2_FAIL`

原实现中 `run_gateway_health()` 曾在 L1 通过后直接走：

```c
if (inject_failure) {
    puts("INJECT: forced L2 failure");
    goto stop;
}
```

这使 L2 失败的原因是**测试开关**，而不是 Gateway 的真实行为，无法证明北向链路真的坏了。

### 3.2 实际采用的注入

测试协调器先 `bind()` 北向上游端口但**不调用 `listen()`**：

```python
holder = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
holder.bind((HOST, 0))          # 只 bind，不 listen
fault_port = holder.getsockname()[1]
```

此时该端口在内核中的监听标志 `SO_ACCEPTCONN == 0`。任何进程对它 `connect()` 都会收到内核返回的 `ECONNREFUSED`（errno 111）。

OTA Agent 通过 `OTA_UPSTREAM_PORT` 拿到该端口后，用**外部上游**模式启动 Gateway：

```
Gateway(新版) --connect()--> 127.0.0.1:<fault_port>  ==>  ECONNREFUSED
```

因此：

- 失败由内核 `connect()` 产生，不是被测程序的预设开关；
- 端口由协调器持有，OTA Agent 自己无法 `accept()`，无法伪造成功；
- 回滚后协调器关闭 holder，旧版 Gateway 改用全新的正常 Mock Server。

外部观测确认（协调器自己 `connect` 该端口）：

```
port=45709, SO_ACCEPTCONN=0
connect errno: 111 ECONNREFUSED
```

---

## 4. 执行流程与证据

```
$ OTA_UPSTREAM_PORT=45709 ./bsp/apps/ota_agent/ota_agent_x86 \
      upgrade-test build/ota-test/real-x86/v0.1.1-test1 \
                   build/ota-test/install-x86-fault.2e0Csr
```

| 阶段 | 日志证据 |
| --- | --- |
| 外部上游指定 | `External upstream port: 45709 (not listening)` |
| 候选包校验 | `PASS: OTA package verification successful`（v0.1.1-test1，SHA-256 `64a39294…6e44`） |
| 旧版校验 | `Version : 0.1.0-test1`，SHA-256 `4970cb84…548a` |
| 切换 `current` | `PASS: candidate activated: 0.1.1-test1` |
| 新版启动 | `Gateway started: pid=42743` |
| **L1 通过** | `PASS: L1 TCP ready` |
| **南向接收成功** | `PROBE: southbound DATA seq=651455336`<br>`PASS: southbound DATA accepted`<br>Gateway 侧：`[WIFI DATA] node=OTA01 seq=651455336 fields=T=253,H=601`<br>`[QUEUE] upstream enqueued, byte=78` |
| **北向真实失败** | `FAIL: northbound refused (ECONNREFUSED) port=45709`<br>Gateway 侧：`[TCP] tcp_connect failed: 127.0.0.1:45709` ×6 |
| **L2 失败** | `FAIL: no northbound business ack within L2 window`<br>`FAIL: L2 business health` |
| 新版进程回收 | `Gateway stop result=0, elapsed=60 ms`（SIGTERM 优雅退出） |
| 回滚意图持久化 | `RECOVERY: state=ACTIVATED, current=releases/v0.1.1-test1` |
| 文件系统回滚 | `RECOVERY: current restored`<br>`PASS: rollback completed` |
| 旧版重新启动 | `Gateway started: pid=42775`（第二次） |
| **旧版 L1/L2 恢复** | `PASS: L1 TCP ready`<br>`PASS: southbound DATA accepted`<br>`PASS: L2 probe forwarded, seq=884653787`<br>`PASS: L2 business health`<br>`PASS: old Gateway business recovered` |
| 事务退出码 | `exit=1`（非零） |

### Gateway 侧统计（旧版回滚后）

```
TCP sent           : 1        <- 南向数据成功经北向发出
TCP send errors    : 0
TCP dropped        : 0
TCP connect errors : 0
```

对比新版（故障期间）：

```
TCP sent           : 0
TCP send errors    : 0
TCP dropped        : 0
TCP connect errors : 6        <- 真实连接失败计数
```

---

## 5. 验收指标核对

| 验收指标 | 预期结果 | 实测 | 证据 |
| --- | --- | --- | --- |
| 新版启动 | 成功 | PASS | `Gateway started: pid=42743` |
| 新版 L1 检查 | PASS | PASS | `PASS: L1 TCP ready` |
| 北向 TCP 连接 | 真实失败 | PASS | `FAIL: northbound refused (ECONNREFUSED) port=45709` + Gateway `tcp_connect failed` ×6 |
| 南向 DATA 接收 | 成功 | PASS | `PASS: southbound DATA accepted`；Gateway `[WIFI DATA]` + `[QUEUE] upstream enqueued` |
| 新版 L2 检查 | FAIL | PASS | `FAIL: L2 business health` |
| 新版进程停止与回收 | 成功 | PASS | `Gateway stop result=0, elapsed=60 ms` |
| 文件系统回滚 | PASS | PASS | `PASS: rollback completed` |
| 旧版重新启动 | 成功 | PASS | 启动计数 = 2 |
| 旧版 L1/L2 检查 | PASS | PASS | `PASS: old Gateway business recovered` |
| OTA 最终状态 | `ROLLED_BACK` | PASS | `state=ROLLED_BACK` |
| 最终确认版本 | `0.1.0-test1` | PASS | `Confirmed : 0.1.0-test1` |
| `current` | `releases/v0.1.0-test1` | PASS | `Current   : releases/v0.1.0-test1` |
| 本次升级退出码 | 非零 | PASS | `exit=1` |

### 运行后状态

```
State     : ROLLED_BACK
Confirmed : 0.1.0-test1
Candidate : 0.1.1-test1
Current   : releases/v0.1.0-test1
Release   : CONFIRMED_TARGET
```

### 资源清理

| 检查项 | 结果 |
| --- | --- |
| 遗留 Gateway 进程 | 无（扫描 `/proc/*/exe`，0 个） |
| TCP 7000 | 已释放，可重新绑定 |
| 两个发行版文件 | SHA-256 与测试前完全一致 |
| 正常升级基线 `install-x86.9L0EFT` | 未被修改（仍为 `CONFIRMED` / `0.1.1-test1`） |

---

## 6. 发现的代码缺陷与修复

### 缺陷 1：L2 探测顺序使北向故障掩盖南向结果

**位置**：`bsp/apps/ota_agent/ota_upgrade_run.c` · `probe_l2()`

**原实现**：先 `accept()` 北向连接成功，然后才 `connect()` Gateway 的 7000 端口发送南向 DATA。

```c
/* 等待 Gateway 的 Server Worker 建立上游连接 */
if (wait_readable(listenfd, deadline) != 0)
    goto out;
upstream = accept4(listenfd, NULL, NULL, SOCK_CLOEXEC);
...
client = socket(...);                          /* 之后才发南向数据 */
```

**影响**：当北向不可达时，`accept()` 永远等不到连接，函数在**发送南向数据之前**就返回失败。于是无法区分"南向接收也坏了"和"只有北向坏了"，本次验收要求的"南向成功 + L2 失败"组合无法被证明。

**修复**：拆成两个独立阶段：

```c
/* ---- 阶段一：南向入站（与北向状态解耦） ---- */
if (send_southbound_data(seq) != 0) { ... return -1; }
puts("PASS: southbound DATA accepted");

/* ---- 阶段二：北向 ---- */
if (listenfd >= 0) { /* 内部 mock：accept 并校验回传 JSON */ }
else               { /* 外部上游：如实耗尽 L2 窗口 */ }
```

### 缺陷 2：故障注入用开关短路，不是真实链路失败

**位置**：同上 · `run_gateway_health()`

**原实现**：

```c
const char *inject = getenv("OTA_TEST_FORCE_L2_FAIL");
if (inject_failure) { puts("INJECT: forced L2 failure"); goto stop; }
```

**影响**：L2 失败纯粹由环境变量决定，Gateway 的北向连接可能完全正常，测试毫无说服力。

**修复**：删除该短路，改为新增 `OTA_UPSTREAM_PORT`：

- 有该变量 → 不创建内部 Mock Server，把该端口作为 Gateway 的 `--port` 参数；
- 端口由协调器 `bind` 但不 `listen`，Gateway 的 `connect()` 得到内核级 `ECONNREFUSED`；
- L2 判定仍走同一套真实探测代码，成败由 Gateway 行为决定。

### 缺陷 3：`expect_l2_success` 被误用为"注入开关"

**位置**：同上 · 回滚后的旧版健康检查

**原实现**：回滚后调用 `run_gateway_health(..., 0)`，而参数语义是 `inject_failure`。修改语义后若沿用 `0`，会被解释为"期望 L2 失败"，导致旧版检查即使成功也被判为失败。

**修复**：参数重命名为 `expect_l2_success`，回滚恢复调用传 `1`；并且把两个方向都做成硬判定：

```c
if (expect_l2_success && !l2_ok)  { puts("FAIL: L2 unexpectedly failed"); goto stop; }
if (!expect_l2_success && l2_ok)  { puts("FAIL: L2 unexpectedly succeeded"); goto stop; }
```

### 缺陷 4：外部上游场景下提前返回，未真实耗尽 L2 窗口

**位置**：同上 · `probe_l2()` 外部上游分支

**现象**：上游"监听但不应答"时，探测在 0.28 s 就返回，而不是等待 `L2_TIMEOUT_MS`（5 s）。虽然结论仍是失败，但**没有给出完整观察窗口**，无法排除"确认只是慢到 1 s 才到"的可能。

**修复**：先把 `connect()` 结果仅作为**故障形态证据**打印，然后统一 `wait_until(deadline)` 耗尽窗口：

```c
if (refused == 1)       printf("FAIL: northbound refused (ECONNREFUSED) port=%u\n", ...);
else if (refused == 0)  printf("NOTICE: upstream %u accepts connections but no ack is expected from it\n", ...);
else                    printf("NOTICE: northbound probe inconclusive, port=%u\n", ...);

wait_until(deadline);   /* 如实耗尽业务确认窗口 */
puts("FAIL: no northbound business ack within L2 window");
```

**修复后实测**：监听但不应答场景耗时从 0.28 s 提升到 **5.32 s**，与 5 s 的 L2 窗口一致。

### 缺陷 5（辅助，非 OTA 代码）：Gateway stdout 全缓冲

Gateway 的 `printf` 在 stdout 重定向到管道时使用全缓冲，日志会被攒到进程退出才输出，导致无法观察时序。测试协调器用 `stdbuf -oL -eL` 包装解决，未改动 Gateway 代码。

---

## 7. 交叉验证

除了主用例（bind 未 listen → `ECONNREFUSED`），额外验证了以下场景：

| 场景 | 上游状态 | 耗时 | 结果 |
| --- | --- | --- | --- |
| A. 监听但不应答 | `listen()` 后 accept 但不回复 ACK | 5.32 s | L2 因超时失败，回滚成功 |
| B. 重复执行 | 从 `ROLLED_BACK` 再次升级 | 5.25 s | 行为一致，仍回滚到 `ROLLED_BACK` |

场景 B 说明事务可从 `ROLLED_BACK` 重入且不残留进程/端口。

---

## 8. 尚未覆盖的风险

以下几点**没有**被本次测试覆盖，不得把结果等同于真实物理故障验收：

1. **真实物理断网**：本次是环回口上的内核 `ECONNREFUSED`，未经过物理网卡、交换机与链路层。真实断网的表现可能是 `EHOSTUNREACH`、静默丢包或长时间 `connect` 挂起，行为与拒接不同。
2. **静默丢包（黑洞）**：本实现中 Gateway 的 `connect()` 是阻塞调用且无超时（`gateway/tcp_client.c` 的 `tcp_client_connect`）。若对端静默丢包，`connect` 可能阻塞数十秒，此时 OTA 的 `STOP_TIMEOUT_MS`（3 s）会升级为 SIGKILL。该路径未被测试。
3. **断电/崩溃恢复**：未测试在 `ROLLBACK_PENDING` 或 `ACTIVATED` 状态下突然断电后，`recover` 能否幂等完成。
4. **并发 OTA**：未测试两个 upgrade 事务同时争抢同一安装目录（依赖 `.ota.lock` 的行为）。
5. **电源与存储异常**：未测试磁盘写满、`fsync` 失败等持久化错误路径。
6. **真实业务服务端**：回滚后使用的是一个仅校验 JSON 的 Mock Server，而非真实业务后端，未验证真实业务语义。
7. **ARM 目标板**：本次在 x86-64 上用 `-DOTA_TARGET_X86_64` 构建；ARMv7 实际二进制与硬浮点 ABI 约束未在本次路径中重新验证。
8. **`connect()` 无超时（真实缺陷，未修复）**：`tcp_client_connect()` 使用阻塞 `connect`，缺少 `ETIMEDOUT` 保护。本次故障注入均为快速拒接，因此未暴露。建议后续改为非阻塞 connect + poll 超时。

---

## 9. 复现方式

```bash
# 1. 构建 x86 测试版 Agent
cd bsp/apps/ota_agent && make ota_agent_x86 && cd -

# 2. 执行验收（--prepare 会恢复 CONFIRMED/旧版 current 初始态）
python3 tests/test_ota_northbound_fault.py --prepare

# 3. 查看完整事务日志
cat build/ota-test/d23-6-8-northbound-fault.log
```

退出码 0 表示 28 项全部通过。

---

## 10. 结论

- 新版 L1 成功、北向 TCP **真实失败**（内核 `ECONNREFUSED`）、南向 DATA 成功解析并入队、L2 因拿不到北向业务确认而失败。
- OTA 自动完成回滚：进程回收、`ROLLBACK_PENDING` → `ROLLED_BACK` 持久化、`current` 恢复到 `releases/v0.1.0-test1`。
- 旧版重新启动后 L1/L2 全部恢复，退出码非零。
- 资源全部清理，正常升级基线 `install-x86.9L0EFT` 未受影响。

**本阶段通过条件已满足。**
