# 北向 TCP connect 阻塞与线程退出延迟治理

> 工程：SmartGateway-BSP
> 范围：`gateway/tcp_client.c`、`gateway/server_link.c`、`gateway/gateway_app.c`
> 日期：2026-11-05
> 结论：阻塞 connect 导致的 18 秒退出延迟已消除（**18.192s → 0.003s**）

---

## 1. 问题定位：四个环节的因果关系

要理解为什么 SIGTERM 之后进程会卡十几秒，必须把下面四点串起来看。

### 1.1 `tcp_client_connect()` 是无界阻塞

原实现（`gateway/tcp_client.c`）直接对**阻塞 socket** 调用 `connect()`：

```c
client_socket_fd = socket(AF_INET, SOCK_STREAM, 0);   /* 阻塞 socket */
...
int ret = connect(client_socket_fd,
                  (const struct sockaddr *)&serveraddr,
                  sizeof(serveraddr));
```

阻塞 `connect()` 的行为：

| 对端状态 | 结果 |
| --- | --- |
| 端口无监听 | 内核立即返回 `ECONNREFUSED`（实测 0 ms） |
| 不可达 | 依据 ICMP 返回 `EHOSTUNREACH` / `ENETUNREACH` |
| **静默丢包（黑洞）** | **一直挂到内核 SYN 重传耗尽** |

实测黑洞地址（`192.0.2.1` 等 TEST-NET 保留段）：

```
connect(192.0.2.1:9000)   elapsed = 21053 ms
connect(10.255.255.1:9000) elapsed = 21040 ms
connect(127.0.0.1:45999)  elapsed = 0 ms   (拒接)
```

即单次 `connect()` 最长占 18~21 秒。

### 1.2 `server_link.c` 的退出标志检查点被 connect 挡住

`server_link_worker` 的主循环结构是：

```c
while (*runningg)                    /* 检查点 A */
{
    message_queue_try_pop(...);       /* 非阻塞 */
    if (context->tcp_fd < 0)
    {
        gateway_app_try_tcp_connect(context);   /* ← 可能阻塞 21 秒 */
        if (context->tcp_fd < 0)
        {
            server_link_short_wait();           /* 100 ms 整块 nanosleep */
            continue;                           /* 回到检查点 A */
        }
    }
    ...poll(tcp_fd, 100ms)...
}
```

**关键事实**：`connect()` 期间循环体不返回，检查点 A 无法被执行。于是 `*running == 0` 这个条件在 21 秒内根本得不到求值机会。

### 1.3 信号不会投递到 worker 线程（决定性原因）

`main.c` 的信号处理：

```c
static volatile sig_atomic_t g_running = 1;

static void handle_signal(int signal_number) { g_running = 0; }

sigaction(SIGINT,  &action, NULL);
sigaction(SIGTERM, &action, NULL);
```

进程收到 SIGTERM 时，内核只把信号投递给**未屏蔽该信号的任意一个线程**；实践中就是主线程。`handle_signal()` 立即返回、`g_running` 变 0，主循环 `poll(wifi_listen_fd, 1000)` 在 1 秒内醒来，进入清理流程：

```c
message_queue_shutdown(&upstream_queue);
pthread_join(bluetooth_thread, NULL);
wifi_worker_group_wait(&wifi_workers);
pthread_join(server_thread, NULL);     /* ← 在这里被 connect 拖住 */
```

**`server_link_worker` 从未收到这个信号**。它不会被打断，也无法主动感知，只能依赖 `*running` 标志。而标志检查点被 `connect()` 挡住 —— 这就是 18 秒延迟的完整因果链：

```
SIGTERM → 仅主线程 handler 置 g_running=0
        → 主线程进清理, pthread_join(server_thread)
        → server_link_worker 仍卡在阻塞 connect() 里
        → 必须等内核 SYN 重传耗尽(18~21s)
        → 主线程 join 才能返回 → 进程退出
```

这一点很重要：**靠信号打断 connect 是走不通的**，必须让 connect 本身可被轮询中断。

### 1.4 其它延迟来源排查（均非主因）

| 位置 | 机制 | 实测影响 |
| --- | --- | --- |
| `server_link_short_wait()` | 100 ms 整块 `nanosleep` | ≤ 100 ms |
| `bluetooth_worker.c` `sleep(1)` | 串口重连退避 | **0 ms**（`sleep` 被信号打断，且主线程已置标志） |
| `tcp_client_send_all()` | `poll(POLLOUT, 1000ms)` | ≤ 1000 ms |
| 主循环 `poll(wifi_listen_fd, 1000)` | 空闲等待 | ≤ 1000 ms |
| `wifi_worker_group_wait()` | 条件变量，等各 worker 退出 | 各 worker 每 100 ms 检查一次 `running`，≤ ~100 ms |
| **`tcp_client_connect()`** | **阻塞 connect** | **18~21 s（主因）** |

### 1.5 重连退避机制

`gateway_app_try_tcp_connect()` 用 1 秒节流：

```c
if (now < context->next_tcp_retry) return;   /* 距上次不足 1 秒则跳过 */
...
context->next_tcp_retry = now + 1;           /* 失败后顺延 1 秒 */
```

外层再叠加 `server_link_short_wait()` 的 100 ms。
所以"快速拒绝"场景下的重试节奏约为 1 秒一次 —— 实测 2.5 秒内 3 次失败，符合预期。

---

## 2. 优化前基线

保留基线二进制 `/tmp/smart_gateway_baseline`（对应提交前 `build/smart_gateway`）。

> 基线源码副本：`/tmp/baseline_tcp_client.c`、`/tmp/baseline_server_link.c`。
> 二进制摘要 `dae54e1f…9b8592`，86184 B。

### 测量方法

`tests/measure_gateway_shutdown.py`：

1. 启动 Gateway，指向指定上游；
2. 等待 `--settle` 秒使其进入稳定状态（可能正卡在 connect）；
3. 发 `SIGTERM`；
4. 计时到进程真正被 `wait()` 回收，超时 60 秒则升级 SIGKILL 并标记。

### 基线数据

| 场景 | 上游 | 退出延迟 |
| --- | --- | --- |
| 快速拒绝 | `127.0.0.1:45999` | **0.007 s** |
| **黑洞地址** | `192.0.2.1:9000` | **18.192 s** |

18 秒完全由阻塞 `connect()` 贡献，与 §1.3 的因果链一致。

---

## 3. 实现：有界可中断 connect

### 3.1 新增接口

`gateway/tcp_client.h` 保持旧函数不变，新增一个：

```c
int tcp_client_connect(const char *server_ip, uint16_t port);

int tcp_client_connect_stoppable(
    const char *server_ip,
    uint16_t port,
    int timeout_ms,
    const volatile int *running
);
```

- `tcp_client_connect()` 变为一行包装，内部调用 `_stoppable` 并传入默认超时
  `TCP_CLIENT_CONNECT_TIMEOUT_MS`（3000 ms）、`running = NULL`。
- **旧签名、旧语义、旧返回值全部保留**，`tests/test_tcp_client.c` 与
  `tests/test_tcp_loop.c` 无需修改。

### 3.2 核心实现 `tcp_client_connect_bounded()`

```c
static int tcp_client_connect_bounded(
    int socket_fd,
    const struct sockaddr_in *address,
    int timeout_ms,
    const volatile int *running)
```

步骤：

1. **先切非阻塞**，再 `connect()`。
   阻塞 socket 上的 connect 无法被 poll 提前打断；非阻塞 connect 立刻返回
   `EINPROGRESS`，由 poll 负责等待。
2. `connect()` 直接返回 0 → 立即成功（环回常见），恢复阻塞模式后返回。
3. `errno != EINPROGRESS` → 直接失败上报（覆盖 `EAGAIN`、`ENETUNREACH`、
   `ECONNREFUSED`，以及被信号打断的 `EINTR`）。
4. 进入轮询循环，每片最多 `TCP_CLIENT_CONNECT_POLL_SLICE_MS`（100 ms）：

```c
for (;;) {
    if (running != NULL && !(*running)) { errno = EINTR; return -1; }  /* ① 停止检查 */

    remaining = deadline - tcp_client_monotonic_ms();                  /* ② 单调时钟 */
    if (remaining <= 0) { errno = ETIMEDOUT; return -1; }
    if (remaining > SLICE) remaining = SLICE;

    poll_result = poll(&pfd, 1, (int)remaining);

    if (poll_result < 0) {
        if (errno == EINTR) continue;    /* ③ 不重置 deadline */
        return -1;
    }
    if (poll_result == 0) continue;      /* 分片到期，回到 ① 查停止标志 */

    /* ④ 用 SO_ERROR 取真实结果，而不是看 revents */
    getsockopt(socket_fd, SOL_SOCKET, SO_ERROR, &so_error, &error_len);
    if (so_error != 0) { errno = so_error; return -1; }

    tcp_client_set_blocking(socket_fd);
    return 0;
}
```

### 3.3 各错误码的处理

| errno | 触发点 | 处理 |
| --- | --- | --- |
| `EINPROGRESS` | 非阻塞 connect 已发起 | 进入 poll 等待 |
| `EINTR`（poll） | 等待被信号打断 | `continue`，**deadline 不变** |
| `EINTR`（返回给调用者） | ① 处检测到 `running == 0` | 立即返回，让线程尽快退出 |
| `EAGAIN` | 本地端口耗尽等 | 直接失败上报 |
| `ETIMEDOUT` | 分片循环耗尽总超时 | 直接失败上报 |
| `ECONNREFUSED` | `SO_ERROR` 读出 | 直接失败上报（保持原有"快速失败"体验） |
| 其它系统错误 | `poll`/`getsockopt` 失败 | 直接失败上报 |

**关于 `EINTR` 不重置超时的说明**：如果每次 `poll` 被打断都重新计算完整超时，
在高信号频率下 `connect` 永不超时。因此 `deadline` 只在进入循环时算一次，
基于 `CLOCK_MONOTONIC`，与系统时间调整无关。

### 3.4 资源释放

所有失败路径统一收敛到 `tcp_client_connect_stoppable()` 末尾：

```c
if (tcp_client_connect_bounded(...) != 0) {
    int save_error = errno;
    close(client_socket_fd);      /* 唯一关闭点 */
    errno = save_error;           /* close 不覆盖真实错误码 */
    return -1;
}
```

`connect()` 立即成功但 `set_blocking` 失败、`poll` 失败、`getsockopt` 失败、
超时、被停止 —— 全部经由这条路径关闭 socket。

---

## 4. 实现：让线程可及时退出

### 4.1 连接路径接入停止标志

`gateway_app_try_tcp_connect()` 增加参数：

```c
void gateway_app_try_tcp_connect(
    gateway_context_t *context,
    const volatile int *running);
```

- 进入前若 `running != NULL && !*running`，**不再发起新连接**，
  避免退出路径上又开一次可能很长的 connect。
- 把 `running` 透传给 `tcp_client_connect_stoppable()`，
  使 connect 的每个 100 ms 分片都能感知退出。
- 失败时日志补充 `strerror(saved_error)`，保留错误码到 `errno`。

### 4.2 退避等待改为分片可中断

```c
static void server_link_short_wait(const volatile int *running)
{
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 10000000L };  /* 10 ms */
    int slices = 10;

    while (slices-- > 0) {
        if (running != NULL && !(*running)) return;   /* 每 10ms 检查 */
        nanosleep(&delay, NULL);
    }
}
```

原本一次 100 ms 的 `nanosleep` 期间完全不看退出标志；
现在拆成 10 片，最坏退出延迟从 100 ms 降到 10 ms。

### 4.3 超时上限与退出响应的关系

| 参数 | 值 | 作用 |
| --- | --- | --- |
| `TCP_CONNECT_TIMEOUT_MS` | 3000 ms | 单次 connect 最坏占用（无 SIGTERM 时） |
| `TCP_CLIENT_CONNECT_POLL_SLICE_MS` | 100 ms | 退出响应粒度 |
| `server_link_short_wait` 分片 | 10 ms | 退避期间退出响应粒度 |
| `SERVER_LINK_POLL_TIMEOUT_MS` | 100 ms | 主循环对下行的响应粒度 |

结论：**退出响应时间不再由 connect 超时长度决定**，而是由 100 ms 分片决定。
即使超时设为 30 秒，SIGTERM 后仍能在 ~100 ms 内退出。

---

## 5. 验证结果

### 5.1 退出延迟对比

| 场景 | 优化前 | 优化后 | 改善 |
| --- | --- | --- | --- |
| 黑洞地址 `192.0.2.1:9000` | **18.192 s** | **0.003 s** | **约 6000 倍** |
| 快速拒绝 `127.0.0.1:45999` | 0.007 s | 0.007 s | 无变化 |

### 5.2 在 connect 进行中的不同时刻发 SIGTERM

`--settle` 分别取 0.5 / 1.0 / 1.5 / 2.0 / 2.5 秒（此时 connect 仍在进行）：

```
settle=0.5s  -> 0.003s
settle=1.0s  -> 0.003s
settle=1.5s  -> 0.008s
settle=2.0s  -> 0.008s
settle=2.5s  -> 0.007s
```

证明退出不是"恰好超时已结束"，而是 connect 被真正中断。

### 5.3 有界 connect 单元测试

`tests/test_tcp_connect_bounded.py` + `tests/test_tcp_connect_bounded.c`，6/6 通过：

| 用例 | 结果 |
| --- | --- |
| 成功连接，socket 恢复阻塞模式 | `rc=0 fd_ok=1 blocking=1` |
| 连接被拒 | `errno=111 (ECONNREFUSED)` |
| 黑洞地址 800 ms 超时 | `elapsed=802ms, errno=110 (ETIMEDOUT)` |
| 黑洞地址 1500 ms 超时 | `elapsed=1503ms, errno=110` |
| 停止标志中断（超时设 5000 ms） | `elapsed=202ms, errno=4 (EINTR)` |
| 64 次失败后 fd 不泄漏 | `before=3 after=3 leak=0` |

### 5.4 行为兼容性

| 检查项 | 结果 |
| --- | --- |
| `make test` 单元测试 10 项 | 全部 PASS |
| `make test-tcp` 集成测试 | PASS |
| `make test-asan`（ASan+UBSan） | 全部 PASS |
| `make test-tsan`（ThreadSanitizer） | 全部 PASS |
| OTA L2 端到端 | `PASS: OTA L2 business health` |
| OTA 自动回滚（28 项） | 28/28 PASS，`ROLLED_BACK` 语义不变 |
| 南向 DATA → 北向转发 | 与基线**逐字节一致** |
| `--version` / 参数解析 | 未变化 |
| 重连退避 | 2.5 s 内 3 次失败（≈1 s 间隔），上游恢复后自动连上并成功转发 |

数据平面比对（优化前后输出相同）：

```json
{"node":"OTA01","seq":12345,"type":"DATA","fields":{"T":"253","H":"601"}}
```

---

## 6. 保持不变的部分（任务 4 约束）

- **JSON Lines 协议**：帧边界仍由 `line_parser_feed()` 按 `\n` 切分，无改动。
- **消息队列**：`message_queue_try_pop/push/shutdown` 语义不变。
- **DATA/CMD/ACK/NACK 业务**：`gateway_app_on_wifi_line` / `on_tcp_line` 未改。
- **OTA 状态语义**：`CONFIRMED` / `ROLLED_BACK` 及状态机转换表未改。
- **连接成功后的收发接口**：返回的 fd 仍是**阻塞模式**，`tcp_client_send_all()`
  与后续 `tcp_client_set_nonblocking()` 的调用顺序与语义都不变。
  `server_link_worker` 仍在连上后显式切非阻塞，与旧行为完全相同。
- **`tcp_client_connect()` 签名**：未变，既有调用方与测试无需修改。

---

## 7. 复现方式

```bash
# 构建
make

# 1) 有界 connect 单元测试
python3 tests/test_tcp_connect_bounded.py

# 2) 退出延迟测量（黑洞地址，connect 会长时间等待）
python3 tests/measure_gateway_shutdown.py --server 192.0.2.1 --port 9000 --settle 3

# 3) 回归
make test && make test-tcp && make test-asan && make test-tsan
python3 tests/test_ota_health_e2e.py
python3 tests/test_ota_northbound_fault.py --prepare
```

---

## 8. 尚未覆盖的风险

1. **DNS 解析阻塞未治理**：`tcp_client_connect` 用 `inet_aton` 只接受点分 IPv4，
   当前不会触发 DNS。若将来支持域名，`getaddrinfo()` 是独立阻塞点，需另行治理。
2. **未在真实物理断网下验证**：本次黑洞地址是 TEST-NET 保留段，链路上无 ICMP
   回执。真实网络若返回 ICMP unreachable，会更快失败；若是中间设备静默丢包，
   表现与本次一致。
3. **上游恢复后首条数据可能延迟**：退避节流是 1 秒，最坏情况下游恢复后
   需等 ≤1 s 才重连。
4. **`send_all` 期间退出**：`poll(POLLOUT, 1000ms)` 不含 `running` 检查，
   SIGTERM 后最坏多等 1 秒。本次未纳入改造（属发送路径，非连接路径）。
5. **ARM 目标板未复测**：本次在 x86-64 主机验证；`CLOCK_MONOTONIC`、`poll`、
   非阻塞 connect 在 Linux 4.9 ARMv7 上语义一致，但未上板实测。
