# D16：i.MX6ULL GDB/gdbserver 远程调试与多线程链路验收记录

> 日期：2026-10-23  
> 工程：SmartGateway-BSP（100ASK i.MX6ULL，ARMv7 Linux）  
> 分支：`feature/bsp-imx6ull`  
> 记录类型：**实机调试验收**。本记录依据 Ubuntu GDB、i.MX6ULL gdbserver 和 NODE04 实际运行日志。  
> 教学版操作说明：[ARM GDB 远程调试零基础教程](tutorial-gdbserver-arm-remote-debugging.md)。  
> **重要限制**：此次提供的 GDB 日志证明了 Gateway 端 TCP 发送函数成功，尚未提供本次同一消息在 Ubuntu Server 端完成业务处理的新日志；勿将其误写为本轮 Server 端验收 PASS。此文档的发布也不代表本地未推送的 Agent/NodeStore 等源码已同步到 GitHub。

## 1. D16 目标和结论

本阶段在 D15 AP3216C Sensor Agent 联调基础上，使用 `gdb-multiarch + gdbserver` 完成：

1. 确认 ARM 调试 ELF 包含 DWARF 信息，且板端可以运行。
2. 远程连接 ARMv7 进程，在 C 源码设置断点、读取寄存器和函数参数。
3. 在 NODE04 SAMPLE 处理现场还原 `wifi_client_worker() → line_parser_feed() → gateway_app_on_wifi_line()` 调用链。
4. 通过 ARM EABI 的 `r7` 寄存器识别 `poll` 系统调用、查看 `pollfd`，定位线程职责。
5. 验证 NODE04 与 WiFi 连接完成身份绑定、DATA 转换为 JSON、经消息队列跨线程转交给 Server Link Worker。
6. 验证 Gateway 通过北向 TCP 连接完成一次 `tcp_client_send_all()` 成功调用。

**判定：D16 的 GDB 操作、线程定位、节点绑定、跨线程消息转发与 Gateway 端发送均 PASS；同次实验的 Ubuntu Server 应用层接收日志待补充。** 这不是故障修复报告，也没有证据声称发现或修复了内存损坏/死锁。

## 2. 实验环境

| 项目 | 实测结果 |
| --- | --- |
| Ubuntu 主机 | `ubuntu-VMware-Virtual-Platform`，`192.168.10.2` |
| Ubuntu 调试器 | `/usr/bin/gdb-multiarch`，GNU GDB 17.1 |
| 可选交叉 GDB | Linaro `arm-linux-gnueabihf-gdb` |
| 开发板 | 100ASK i.MX6ULL，`uname -m` 为 `armv7l`，IP `192.168.10.3` |
| 板端工具 | `/usr/bin/gdbserver`，亦有 `/usr/bin/gdb` |
| ARM ELF | `build/arm-gdb/smart_gateway` |
| 板端部署路径 | `/tmp/smartgateway-gdb` |
| ARM 动态链接器 | `/lib/ld-linux-armhf.so.3 -> ld-2.30.so` |
| Gateway 南向端口 | TCP 7000，NODE04 Agent 使用本地 TCP 连接 |
| Gateway 北向端口 | Ubuntu Server `192.168.10.2:9000` |
| GDB 远程端口 | 板端 `192.168.10.3:2345` |
| 本次业务消息 | NODE04，`seq=1`，`DEV=5`，`LIGHT_RAW=0`，`KIND=SAMPLE` |

IP、工作目录和路径为本次实验实际配置，并不是所有开发环境中的固定值。gdbserver 不应暴露到公网。

## 3. 调试 ELF 及工具链检查 — PASS

Ubuntu 执行：

~~~sh
file build/arm-gdb/smart_gateway
readelf -S build/arm-gdb/smart_gateway | grep debug
command -v gdb-multiarch
command -v arm-linux-gnueabihf-gdb
gdb-multiarch --version
~~~

实测 ELF：

~~~text
ELF 32-bit LSB executable, ARM, EABI5, dynamically linked,
interpreter /lib/ld-linux-armhf.so.3,
with debug_info, not stripped
.debug_aranges
.debug_info
.debug_abbrev
.debug_line
.debug_frame
.debug_str
.debug_loc
.debug_ranges
.debug_macro
~~~

`gdb-multiarch build/arm-gdb/smart_gateway` 可加载符号：

~~~gdb
info files
info functions gateway_app_on_wifi_line
~~~

实际识别 `gateway/gateway_app.c:1103` 的函数定义。`-g` 提供 DWARF 源码/类型/变量调试信息，`-Og` 仍会产生优化；`with debug_info` 与 `not stripped` 均已验证。Perf 显示函数名也可能依赖 ELF 符号表，不能简单说“不加 -g，Perf 一定看不到函数”。

## 4. 部署、连接和函数入口断点 — PASS

Ubuntu 实际上传成功：

~~~sh
scp build/arm-gdb/smart_gateway root@192.168.10.3:/tmp/smartgateway-gdb
# smart_gateway 100% 310KB
~~~

曾误连 `192.168.10.2`（Ubuntu），导致 root 密码拒绝；实际开发板 IP 是 `192.168.10.3`。SCP 的非后量子密钥交换提示属于 SSH 安全告警，传输仍成功；新主机指纹需独立核验，不能无条件接受。

板端（启动前先停止旧 Gateway，避免 7000 端口冲突）：

~~~sh
chmod +x /tmp/smartgateway-gdb
gdbserver 192.168.10.3:2345 /tmp/smartgateway-gdb \
  --no-serial --server 192.168.10.2 --port 9000
~~~

Ubuntu：

~~~sh
gdb-multiarch build/arm-gdb/smart_gateway
~~~

GDB 中**逐行输入，每行回车**：

~~~gdb
set pagination off
set debuginfod enabled off
set sysroot target:
target remote 192.168.10.3:2345
info registers
break gateway_app_on_wifi_line
continue
~~~

实测：初始停止在板端动态链接器 `_start`（`pc=0x76fced00`），可读取 ARM `r0..r12/sp/lr/pc/cpsr`；源码断点：

~~~text
Breakpoint 1 at 0x14688: file gateway/gateway_app.c, line 1108.
~~~

函数定义始于 1103 行，而断点行号是 1108 行：编译器以 DWARF 将机器指令映射到源码；声明行通常没有独立可执行机器指令。初次输入曾把 `set sysroot` 与 `target remote` 粘在一行，导致 GDB 将“target remote ...”误认为 sysroot；拆为两行后成功。GDB 17.1 提示旧 `remote:` 已弃用，本次使用 `target:`。

## 5. NODE04 的真实调用栈与参数 — PASS

Agent 发送第一条消息时，**Thread 2** 命中断点：

~~~text
gateway_app_on_wifi_line(
  line="{\"node\":\"NODE04\",\"seq\":1,\"type\":\"DATA\",\"fields\":{\"DEV\":\"5\",\"LIGHT_RAW\":\"0\",\"KIND\":\"SAMPLE\"}}\n",
  length=92,
  user_data=0x76660e1c)
at gateway/gateway_app.c:1108
~~~

`bt` 还原（由当前函数向调用者回溯）：

~~~text
#0 gateway_app_on_wifi_line()   gateway/gateway_app.c:1108
#1 line_parser_feed()           common/line_parser.c:73
#2 wifi_client_worker()         gateway/wifi_client.c:478
#3 libc / pthread ...           （缺少完整展开信息）
~~~

源码对应：`wifi_client_worker()` 通过 `recv()` 接收 TCP 字节流，再调用 `line_parser_feed(..., gateway_app_on_wifi_line, &app_context)`；行解析器识别换行符之后，通过 `callback(buffer,line_length,user_data)` 进入业务回调。因此 `#0/#1/#2` 是**同一线程的函数调用链**，不是三条线程。

`info args` 验证 NODE04、seq=1、`length=92`；`info locals` 曾见 `<optimized out>` 和未初始化的 `type_name` 字节。这是 `-Og` 优化、断点位于函数入口所致的可能表现，不是 JSON 已损坏的证据。

## 6. ARM SVC、系统调用与线程身份 — PASS

在 `thread apply all bt 8` 中，线程 1、3 的 libc 帧显示 `??` 和 `corrupt stack?` 提示。`info sharedlibrary` 显示 `libpthread.so.0`、`libc.so.6` 缺少已加载的完整调试符号；仅凭回溯失败不可断言栈损坏或死锁。

`thread 1`、`thread 3` 的 `x/8i $pc` 均停在：

~~~asm
=> 0x76f2a534: svc 0x00000000
~~~

ARM 32 位 Linux EABI 在该系统调用入口使用 `r7` 表示系统调用号。本次：

| 线程 | r7 | 识别结果 | r0（pollfd 地址） | r1（nfds） | r2（timeout） | pollfd |
| --- | ---: | --- | --- | ---: | ---: | --- |
| Thread 1 / 5440 | 168 | `poll` | `0x7efe3184` | 1 | 1000 ms | `fd=3, events=1, revents=0` |
| Thread 3 / 10862 | 168 | `poll` | `0x76e61838` | 1 | 100 ms | `fd=4, events=1, revents=0` |

实际 GDB 命令：

~~~gdb
thread 1
info registers r0 r1 r2 r3 r7 pc sp lr
p/d $r7
p *(struct pollfd *)$r0

thread 3
info registers r0 r1 r2 r3 r7 pc sp lr
p/d $r7
p *(struct pollfd *)$r0
~~~

结合源码 `gateway/gateway_loop.c`，主线程执行 `poll(&pollfds,1,1000)`，监听 `wifi_listen_fd`，即 `fd=3`。结合 `gateway/server_link.c`，Server Worker 执行 `poll(&pfd,1,timeout_ms)`，空闲时 100 ms，监测 `context->tcp_fd`，实测 `fd=4`。Thread 3 后续命中 `server_link_worker()` 业务断点，独立证实身份。Thread 2 的 NODE04 连接 `device_fd=5`。

解释：`events=1` 即 `POLLIN`；`revents=0` 是观察时结构体字段值，线程处于系统调用中时不应将其解释为已经完成的 poll 返回状态。正常阻塞等待 I/O 不等于死锁。`lr=0`、缺失调试符号、栈展开失败也不能单独证明栈损坏。

## 7. 回调上下文、设备身份绑定 — PASS

回到 Thread 2 的 frame 0：

~~~gdb
thread 2
frame 0
ptype gateway_app_upstream_context_t
p *(gateway_app_upstream_context_t *)user_data
~~~

实测：

~~~text
gateway_context = 0x7efffb70
upstream_queue  = 0x7efe31c0
device_manager  = 0x7efe52d4
transport       = DEVICE_TRANSPORT_WIFI
device_fd       = 5
bound_node_id   = '\000' <repeats 31 times>
~~~

函数入口时 `bound_node_id` 尚空，表示此时该新连接尚未完成绑定，**不能据此认定错误**。设置一次性断点后继续：

~~~gdb
disable 1
tbreak gateway/gateway_app.c:523
continue
bt 3
p upstream_context->bound_node_id
p node_id
~~~

实测 `gateway_app_touch_device()` 命中第 523 行，`bound_node_id="NODE04"`、`node_id="NODE04"`；此时源码已于第 521 行 `memcpy` 完成绑定。

身份约束：`gateway_app_touch_device()` 后续使用 `strcmp(bound_node_id,node_id)` 拒绝同一连接上的 NODE03 冒用数据。合法换绑须由连接解绑和设备管理器的受控流程完成，不能仅改字符串。回调中的 `void *user_data` 提供模块解耦，但类型/生命周期/同步仍由调用者负责。

## 8. JSON 重新序列化、入队与出队 — PASS

在 Thread 2 继续：

~~~gdb
tbreak gateway/gateway_app.c:1351
continue
p json_length
p json
p upstream_queue
~~~

实测：

~~~text
json_length = 93
upstream_queue = 0x7efe31c0
json = "{\"node\":\"NODE04\",\"seq\":1,\"type\":\"DATA\",\"fields\":{\"DEV\":\"5\",\"LIGHT_RAW\":\"0\",\"KIND\":\"SAMPLE\"}}\n"
~~~

回调输入长度为 **92**（解析器排除换行），重新生成的 JSON Lines 长度为 **93**（包含末尾 `\n`）；不要混淆消息正文长度与网络帧长度。

在 `gateway_app.c:1351` 处尚未执行入队函数，不能直接断言 push 成功。接着在 Server Worker `server_link.c:130` 成功分支命中：

~~~gdb
tbreak gateway/server_link.c:130
continue
info threads
p queue_result
p pending_messsage.length
x/s pending_messsage.data
~~~

实测：

~~~text
Thread 3 hit Temporary breakpoint, server_link_worker(), line 130
queue_result = 0
pending_messsage.length = 93
pending_messsage.data = "{\"node\":\"NODE04\",\"seq\":1,\"type\":\"DATA\",\"fields\":{\"DEV\":\"5\",\"LIGHT_RAW\":\"0\",\"KIND\":\"SAMPLE\"}}\n"
~~~

项目 `message_queue.h`：`MESSAGE_QUEUE_OK=0`；`message_queue_try_pop()` 成功取出同一条 93 字节消息。结合生产者侧断点和消费者侧数据，可确认本条消息经过共享队列传递；但此次没有单独保存 `message_queue_push()` 返回值的 GDB 截图。

`pending_messsage` 是项目源码真实变量名（`messsage` 三个 s）；使用 `pending_message` 会报告 `No symbol`。GDB 对 `P` / `p`、成员名 `tcp` / `tcp_fd` 也严格区分。

## 9. 北向 TCP 发送成功路径 — PASS（Gateway 端）

Server Worker 中设置断点：

~~~gdb
tbreak gateway/server_link.c:225
continue
p context->tcp_fd
p context->tcp_send
p pending_messsage.length
p pending_messsage.data
continue
~~~

实测：

~~~text
Thread 3 hit Temporary breakpoint 5 at gateway/server_link.c:225
context->tcp_fd = 4
context->tcp_send = 1
pending_messsage.length = 93
pending_messsage.data = "{\"node\":\"NODE04\",\"seq\":1,\"type\":\"DATA\",\"fields\":{\"DEV\":\"5\",\"LIGHT_RAW\":\"0\",\"KIND\":\"SAMPLE\"}}\n"
~~~

源码 `gateway/server_link.c`：`tcp_client_send_all() != 0` 会走失败分支、断开并重试；命中第 225 行说明本次发送函数已返回成功，并且 `context->tcp_send++` 已执行。这**不等于**远端 Server 应用已处理；需补 `[NodeStore] node=NODE04 seq=1` 或同等 Server 侧时间对应日志才算本轮端到端验收。

Server Worker 集中拥有北向 Socket，WiFi Worker 不直接 `send()`；既减少跨线程 Socket 状态竞争，也将北向重连、超时、待发送缓存统一管理。队列可能满，`send()` 成功也非远端业务 ACK 或 exactly-once 保证。

## 10. 故障排查清单

| 现象 | 本次原因或优先核查 |
| --- | --- |
| `scp root@192.168.10.2` 密码拒绝 | IP 写成 Ubuntu Server；开发板实际为 `192.168.10.3` |
| `set sysroot remote:target remote ...` | 两条 GDB 命令粘连；分两行执行 `set sysroot target:`、`target remote ...` |
| `Reading /lib/... from remote target...` | GDB 正在读取远端共享库；不代表报错 |
| `<optimized out>` | `-Og` 仍优化；可换 `-O0 -g3` 的独立构建复验 |
| `corrupt stack?` / `??` | 先检查共享库符号、栈展开、寄存器，不能直接断定内存破坏 |
| `No symbol "user_data"` | 需要切到 Thread 2、Frame 0 |
| `Undefined command: "P"` | GDB 命令区分大小写，改用小写 `p` |
| `pending_message` 不存在 | 源码拼写为 `pending_messsage` |
| `context->tcp` 不存在 | 正确成员为 `context->tcp_fd` |
| GDB 命中断点后 Agent 暂停发送 | 默认 all-stop 模式暂停被调试进程的多个线程，属于调试影响 |
| TCP 已发送但 Server 无记录 | 核对 Server 实际接收、节点解析、断线和重复消息；不可仅凭 send 返回值推断业务成功 |

## 11. 验收矩阵与未完成事项

| 验收项 | 状态 | 证据 |
| --- | --- | --- |
| ARM ELF / DWARF | PASS | `file` / `readelf` |
| Ubuntu ↔ 板端 gdbserver | PASS | `target remote` / ARM 寄存器 |
| NODE04 WiFi 回调命中 | PASS | Thread 2、`seq=1` |
| 线程主职责解析 | PASS | ARM `r7=168`、`pollfd`、源码 |
| 设备身份绑定 | PASS | `bound_node_id=NODE04` |
| 消息重新序列化 | PASS | 93 字节 JSON |
| 消息跨线程取出 | PASS | Thread 3、`queue_result=0`、JSON 一致 |
| Gateway 北向发送成功 | PASS | `server_link.c:225`、`tcp_send=1` |
| **本次 Server 应用层接收确认** | **待补证** | 本轮未提供与 `seq=1` 对应的 Server 日志 |
| 错误注入/内存破坏根因修复 | 未开展 | 本轮无确证 Bug，不虚构 RCA |

## 12. 面试可复述总结与下一阶段

> 在 100ASK i.MX6ULL 上，我用 gdbserver 远程调试多线程 Gateway，通过源码断点还原 WiFi Worker 的 TCP 接收与回调链，通过 ARM EABI 的 `r7` 寄存器识别 `poll` 系统调用，并结合 `pollfd`、源码判断主线程与北向 Server Worker 的等待职责。进一步在节点绑定和队列消费者处设置临时断点，确认 NODE04 SAMPLE 在不同线程间完整传递，最后证明北向 `tcp_client_send_all()` 返回成功；明确区分本地成功发送与 Server 应用层确认。

建议后续补充 Server 端日志、编译参数和退出清理的回归检查，再进入 RS485/Modbus 节点与应用 OTA 阶段。`D16` 文档和教程不包含未提交的业务源码；本地代码须由开发者后续单独检查、提交和推送。
