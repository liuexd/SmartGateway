# 从零开始：i.MX6ULL ARM Linux 的 GDB/gdbserver 远程调试教程

> 适用：刚接触 Linux BSP、ARM 交叉编译与用户态多线程调试的开发者。  
> 案例：SmartGateway-BSP，NODE04 AP3216C Sensor Agent → Gateway WiFi Worker → 消息队列 → Server Worker。  
> 实测记录：[D16 GDB 远程调试验收](D16-gdb-remote-debugging.md)。  
> 本文既是**可重做的教学步骤**，也保留实际踩坑与实测证据；“预期输出”不应误认为每台开发板的实际值。

## 0. 先理解：调试谁、在哪执行？

本项目有三类进程：

- **Ubuntu 开发机**：编译和保存源码，运行 `gdb-multiarch`（控制端），本次 IP 为 `192.168.10.2`。
- **i.MX6ULL ARM Linux 开发板**：运行 ARM 版 Gateway 和 `gdbserver`（执行端），本次 IP 为 `192.168.10.3`。
- **NODE04 Sensor Agent**：在板端采集 AP3216C 传感器并连接本地 Gateway TCP 7000。Ubuntu 上的 Server 在 TCP 9000 监听。

通信关系：

~~~text
开发机 Ubuntu                                     开发板 i.MX6ULL
gdb-multiarch   ------ TCP 2345 --------------> gdbserver ----> smartgateway-gdb
源码/带符号 ELF                                  ARM CPU 真正执行机器指令
                                                      ^
                                                      | TCP 7000
                                                NODE04 sensor_agent
                                                      |
                                                      | Gateway 北向 TCP
                                                      v
Ubuntu Server 192.168.10.2:9000 <---------------- Gateway Server Worker
~~~

**不要混淆端口**：2345 是调试端口，7000 是南向节点接入，9000 是北向服务端口。**不要混淆 IP**：192.168.10.2 是 Ubuntu，192.168.10.3 才是板端。请以本机实际 `ip -br -4 addr` 为准。

`gdbserver` 让板上目标进程暂停/单步并将信息传给远端 GDB；GDB 通过本地有 DWARF 符号的 ELF 还原 C 代码、参数与调用栈。默认是 all-stop 调试：一个线程触发断点时其他线程也会暂停。不要把断点造成的网络超时误判为业务故障。

> 本教程以**用户态 Gateway** 为对象，而不是用 GDB 远程调试 Linux 内核；内核调试涉及 kgdb、JTAG 等不同机制。

## 1. 前置条件：避免第一步就卡住

Ubuntu：

~~~sh
cd ~/Desktop/project/SmartGateway-BSP
command -v arm-linux-gnueabihf-gcc
command -v gdb-multiarch
command -v arm-linux-gnueabihf-gdb
~~~

i.MX6ULL：

~~~sh
uname -m
uname -r
command -v gdbserver
ls -l /lib/ld-linux-armhf.so.3
ip -br -4 addr
~~~

本次实测：`uname -m` 为 `armv7l`，`/usr/bin/gdbserver`、`/usr/bin/gdb` 可用，板端动态链接器链接至 `ld-2.30.so`；Ubuntu `gdb-multiarch` 17.1。

**安全/运行提醒**：调试时会占用 Gateway 的 7000 端口；不要同时运行正式版和调试版。`gdbserver` 不具备生产环境所需的强身份认证，不要暴露 2345 到公网；仅在可信隔离 LAN 上运行，调完退出。必要时先确认 Sensor Agent 和 Server 的运行状态，准备好可中断的实验环境。

## 2. 什么是 `-g`、`-O0`、`-Og`、`-fno-omit-frame-pointer`？

- `-g3`：生成更完整的 DWARF 调试信息；有助于 GDB 查看源码、变量和宏信息。
- `-O0`：关闭优化，变量和单步行为通常更接近初学者的源码直觉，但性能/时序与正常版本差别更大。
- `-Og`：面向调试的优化等级，**仍然会优化掉变量**；本次确实出现 `<optimized out>`。
- `-fno-omit-frame-pointer`：尽量保留帧指针，改善部分调用栈展开；ARM 上依然受编译器/ABI/库符号限制。
- `-pthread`：本项目使用 POSIX 线程，编译和链接都应保留。
- `-g` 不是“让 CPU 执行 C 源码”；CPU 始终执行机器指令。Perf 显示符号名通常也能借助 ELF 符号表完成，并非只要有 `-g` 才能显示函数。

本次已经获得 `build/arm-gdb/smart_gateway`，确认 `with debug_info, not stripped`。以后如果需要重建，可以先检查**你本地**的 `makefile` 是否支持 `BUILD_DIR/CC/CFLAGS` 命令行覆盖，再用如下**示例**：

~~~sh
make -f makefile \
  BUILD_DIR=build/arm-gdb \
  CC=arm-linux-gnueabihf-gcc \
  CFLAGS="-Wall -Wextra -Werror -std=c11 -pthread -Og -g3 -fno-omit-frame-pointer" \
  gateway

file build/arm-gdb/smart_gateway
readelf -S build/arm-gdb/smart_gateway | grep debug
~~~

如果 make 报找不到交叉编译器，检查 PATH 或使用全路径 `CC=/绝对路径/arm-linux-gnueabihf-gcc`；如果 make 规则与你本地版本不同，以本地 makefile 为准。务必用单独 BUILD_DIR，避免把主机 x86 目标文件混进 ARM 链接。实际调试程序版本需与**GDB 本地加载的 ELF 一致**；不要传输后在本地重编译而仍调旧进程。

本次检查：

~~~text
ELF 32-bit LSB executable, ARM, EABI5 ... interpreter /lib/ld-linux-armhf.so.3
with debug_info, not stripped
.debug_info
.debug_line
.debug_frame
~~~

如果 `file` 显示 x86-64，而不是 ARM，请先修正编译目标，无法在 ARM 开发板上运行。

## 3. 把可执行文件传到真正的开发板

先在 Ubuntu 与板端分别执行：

~~~sh
ip -br -4 addr
~~~

本次板端地址：`192.168.10.3`；Server/Ubuntu 地址：`192.168.10.2`。

Ubuntu 实际成功命令：

~~~sh
scp build/arm-gdb/smart_gateway \
    root@192.168.10.3:/tmp/smartgateway-gdb
~~~

实际输出：`smart_gateway 100% 310KB`。本次曾误用 `root@192.168.10.2`，登录到了 Ubuntu Server 方向并提示密码被拒绝；这不是 ELF 编译失败。SSH 首次连接的主机指纹应独立确认；“not using a post-quantum key exchange” 是安全提示，不表示本次文件未传成功，后续可在兼容性允许时升级 SSH。

板端：

~~~sh
ls -lh /tmp/smartgateway-gdb
chmod +x /tmp/smartgateway-gdb
file /tmp/smartgateway-gdb  # 若板端安装 file 工具
~~~

没有 SSH/SCP 时，只有在板端已启用 ADB 的前提下才可使用：

~~~sh
adb devices
adb push build/arm-gdb/smart_gateway /tmp/smartgateway-gdb
~~~

注意文件名：本次真实部署的是 `/tmp/smartgateway-gdb`，不是 `/tmp/smart_gateway-gdb`。Linux 文件名不允许凭记忆自动替换。

## 4. 在 ARM 开发板启动 gdbserver

先确认原 Gateway 不占用 TCP 7000，并使用程序自己的正常退出方式停止旧实例（不要未经确认地 `kill -9`）。

板端：

~~~sh
gdbserver 192.168.10.3:2345 /tmp/smartgateway-gdb \
  --no-serial --server 192.168.10.2 --port 9000
~~~

**注意解释**：

- `192.168.10.3:2345`：gdbserver 在板上等待调试控制连接。
- `/tmp/smartgateway-gdb`：gdbserver 管理的 ARM 可执行文件。
- `--no-serial`：禁用本次不需要的蓝牙串口节点。
- `--server 192.168.10.2 --port 9000`：Gateway 本身连接 Ubuntu Server 的业务参数，不是 GDB 参数。

典型预期：

~~~text
Process /tmp/smartgateway-gdb created; pid = ...
Listening on port 2345
~~~

`gdbserver` 会一直占着这个终端，**不要关闭终端**；接下来另开 Ubuntu 终端。

## 5. Ubuntu 通过 GDB 远程连接

Ubuntu：

~~~sh
cd ~/Desktop/project/SmartGateway-BSP
gdb-multiarch build/arm-gdb/smart_gateway
~~~

在 `(gdb)` 提示符内**一行一行输入，每行回车**：

~~~gdb
set pagination off
set debuginfod enabled off
set sysroot target:
target remote 192.168.10.3:2345
~~~

本次用 GDB 17.1，`remote:` 被提示弃用，因此示例使用 `target:`。`set sysroot target:` 是设置从目标读取共享库的位置；**`target remote` 是真正建立调试连接的命令**，不能合成一行。

本次连接后停在：

~~~text
0x76fced00 in _start () from target:/lib/ld-linux-armhf.so.3
~~~

这很正常：动态链接程序启动时先进入动态链接器，不一定立刻停在 `main()`。

初学者验证连接：

~~~gdb
info registers
info files
info functions gateway_app_on_wifi_line
~~~

若能看到 ARM `r0`～`r12`、`sp`、`lr`、`pc` 以及源码函数，说明**远端控制和符号关联都正常**。

> 踩坑：输入 `set sysroot remote:target remote 192.168.10.3:2345` 会把整段误当 sysroot，而不是发起连接。修复：`set sysroot target:` 后按回车，再输入 `target remote 192.168.10.3:2345`。

远程库提示 `Reading /lib/libc.so.6 from remote target...` 通常是 GDB 正在读取共享库，并非直接的故障信号。本轮 `debuginfod` 选 `n` 不影响调试自己的带符号 Gateway。

## 6. 第一个源码断点：NODE04 数据回调

在 GDB：

~~~gdb
break gateway_app_on_wifi_line
continue
~~~

启动/保持板端 `sensor_agent` 向 Gateway 7000 发送数据。第一次命中时，本次实测：

~~~text
Thread 2 hit Breakpoint 1, gateway_app_on_wifi_line(...)
at gateway/gateway_app.c:1108
line = "{\"node\":\"NODE04\",\"seq\":1,\"type\":\"DATA\",\"fields\":{\"DEV\":\"5\",\"LIGHT_RAW\":\"0\",\"KIND\":\"SAMPLE\"}}\n"
length = 92
user_data = 0x76660e1c
~~~

学习命令：

~~~gdb
bt
info args
info locals
list 1103,1130
info threads
thread apply all bt 8
~~~

含义：

| 命令 | 做什么 |
| --- | --- |
| `break 函数名` / `break 文件:行号` | 添加常驻断点 |
| `tbreak 文件:行号` | 仅命中一次的临时断点 |
| `continue` 或 `c` | 继续执行 |
| `next` 或 `n` | 源码级单步，函数调用通常作为一步跳过 |
| `step` 或 `s` | 源码级单步，尝试进入被调用函数 |
| `bt` | 当前线程的函数调用栈 |
| `frame 0` | 选择当前函数栈帧 |
| `info args` / `info locals` | 查看参数 / 局部变量 |
| `info threads` | 列出被调试进程线程 |
| `thread 2` | 切换当前选中线程 |
| `p expression` | 按 C 表达式打印变量/指针成员 |
| `x/s address` | 以内存中的 C 字符串形式查看地址 |
| `disable 1` / `enable 1` | 停用 / 启用编号 1 的断点 |
| `info breakpoints` | 查看断点及编号 |
| `delete 1` | 删除指定断点 |

`bt` 实际输出：

~~~text
#0 gateway_app_on_wifi_line()  gateway/gateway_app.c:1108
#1 line_parser_feed()          common/line_parser.c:73
#2 wifi_client_worker()        gateway/wifi_client.c:478
~~~

**`#0`、`#1`、`#2` 是同一线程的调用层次，不代表三条线程。** 调用过程为 `recv` 收到字节流 → `line_parser_feed` 按换行分出完整 JSON Lines → 将 `gateway_app_on_wifi_line` 作为回调函数指针调用。写 `gateway_app_on_wifi_line` 是传递函数地址；写 `gateway_app_on_wifi_line()` 则是在传参前执行函数，含义完全不同。

函数定义在 1103 行、断点位于 1108 行，是因为 DWARF 映射的是**有机器指令的位置**，并非声明的每一行都能断。

`info locals` 里的 `<optimized out>` 表示变量在该位置可能已被 `-Og` 优化；函数入口处未初始化的字符数组显示乱码，不等于业务内存损坏。

## 7. 调试回调参数 `void *user_data` 与连接身份

项目 WiFi Worker 调用：

~~~c
line_parser_feed(
    wifi_buffer, &wifi_length, sizeof(wifi_buffer),
    gateway_app_on_wifi_line, &app_context
);
~~~

`&app_context` 被作为 `void *user_data` 传递；这样通用解析器不依赖业务全局变量。`void *` 并非天然安全，还必须保证真实类型、生命周期及并发访问正确。

在**Thread 2 + frame 0** 执行：

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
bound_node_id   = 全零
~~~

`bound_node_id` 此时为空，因为回调停在入口，节点绑定可能尚未发生。验证其变化：

~~~gdb
disable 1
tbreak gateway/gateway_app.c:523
continue
bt 3
p upstream_context->bound_node_id
p node_id
~~~

本次在第 523 行看到两个值均为 `"NODE04"`；第 521 行 `memcpy` 已成功保存节点 ID。之后同一连接若声称是 NODE03，`gateway_app_touch_device()` 会做 `strcmp` 身份不匹配判断并拒绝；不可只覆盖 `bound_node_id`，否则会破坏设备管理器中的连接映射。

> 踩坑：先执行 `thread 3` 再打印 `user_data`，得到 `No symbol "user_data"`，不是参数丢失，而是你处在另一个线程/栈帧。必须先 `thread 2`、`frame 0`。

## 8. 多线程看不清调用栈？从 ARM 寄存器反推系统调用

本次 `thread apply all bt 8` 中，Thread 1/3 的 libc 栈帧出现 `??`、`corrupt stack?`。检查：

~~~gdb
info sharedlibrary
thread 1
info registers pc sp lr
x/8i $pc
~~~

实际 `libc.so.6`、`libpthread.so.0` 无可用完整调试符号；两个线程停在：

~~~asm
=> 0x76f2a534: svc 0x00000000
~~~

`svc` 是 ARM 用户态进入内核系统调用的指令。ARM **32 位 EABI** 中，`r7` 通常保存 syscall 号，`r0..r6` 放参数，`r0` 放返回值。

本次每个线程依次执行：

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

得到：

| 线程 | `r7` | `r1` | `r2` | `*(pollfd *)r0` | 源码对应 |
| --- | ---: | ---: | ---: | --- | --- |
| 1 主线程 | 168 = `poll` | 1 | 1000 ms | `fd=3, events=1, revents=0` | `gateway_loop.c` 接受新 WiFi 连接 |
| 3 Server Worker | 168 = `poll` | 1 | 100 ms | `fd=4, events=1, revents=0` | `server_link.c` 等待 Server 下行数据 |
| 2 WiFi Worker | 在业务断点 | — | — | `device_fd=5` | `wifi_client.c` 处理 NODE04 |

例如线程 1 对应：

~~~c
poll(&pollfds, 1, 1000);
~~~

`r0` 是 `struct pollfd` 数组地址、`r1` 是监听文件描述符个数、`r2` 是超时毫秒数；`events=1` 为 `POLLIN`。线程 3 的等待 100ms 是 Server Worker 的空闲轮询，随后已实际命中 `server_link_worker`，证实身份。

**常见误判**：线程位于 `poll()` 表示正常等待网络事件/超时的可能性很高，不能只因 `bt` 显示 `??` 或 `corrupt stack?` 就宣称程序死锁、栈被破坏。`lr=0` 也不是独立的栈损坏证明；要结合符号文件、栈展开与寄存器进一步判断。

## 9. 验证跨线程队列传输和 Gateway 发包

### 9.1 NODE04 的 JSON 重新编码

在 Thread 2：

~~~gdb
tbreak gateway/gateway_app.c:1351
continue
p json_length
p json
p upstream_queue
~~~

实际：

~~~text
json_length = 93
upstream_queue = 0x7efe31c0
json = "{\"node\":\"NODE04\",\"seq\":1,\"type\":\"DATA\",\"fields\":{\"DEV\":\"5\",\"LIGHT_RAW\":\"0\",\"KIND\":\"SAMPLE\"}}\n"
~~~

为什么 `info args` 的 `length=92`，现在 `json_length=93`？因为 `line_parser_feed` 传给回调时按 JSON Lines 去掉结尾换行；`message_json_build_data` 再次编码成可传输的带 `\n` 完整消息。

停在第 1351 行时还没有实际入队，不要仅凭函数即将被调用，就写“入队成功”。

### 9.2 让消费者证明真正出队

Server Worker 源码：

~~~c
queue_result = message_queue_try_pop(queue, &pending_messsage);
if (queue_result == MESSAGE_QUEUE_OK)
    have_pending_message = 1;
~~~

GDB：

~~~gdb
tbreak gateway/server_link.c:130
continue
info threads
p queue_result
p pending_messsage.length
x/s pending_messsage.data
~~~

实际命中 **Thread 3**，`queue_result=0`（`MESSAGE_QUEUE_OK`）、长度 93、JSON 仍为 NODE04 seq=1，说明消费者成功取到了对应消息。这间接佐证了生产者在此前成功入队；如果要单独观察 push 的函数返回值，还可以在 `gateway_app.c:320` 增加断点并检查 `result`。

源码拼写注意：变量是 `pending_messsage`（`messsage` 三个 s），**不是** `pending_message`；GDB 精确匹配真实符号名，不会自动纠正拼写。

### 9.3 TCP 发送成功分支

GDB：

~~~gdb
tbreak gateway/server_link.c:225
continue
p context->tcp_fd
p context->tcp_send
p pending_messsage.length
p pending_messsage.data
continue
~~~

实测 `Thread 3` 停在第 225 行；`tcp_fd=4`、`tcp_send=1`、长度 93、NODE04 seq=1 保持一致。该代码位置位于 `tcp_client_send_all()` 成功返回、`context->tcp_send++` 之后，因此本轮 **Gateway 端发送 PASS**。

**严禁过度推断**：发送调用返回 0，并不能保证 Ubuntu Server 已解码、存储或确认。需要另查 Server `[NodeStore] node=NODE04 seq=1` 等日志才可宣称此轮真正端到端成功。重连 `pending_messsage` 也不能提供 exactly-once 语义。

## 10. 附：GDB 小抄

~~~gdb
help breakpoints
info breakpoints
break gateway_app_on_wifi_line
tbreak gateway/gateway_app.c:523
disable 1
enable 1
delete 1
continue
next
step
finish
bt
bt 8
thread apply all bt 8
info threads
thread 2
frame 0
info args
info locals
p variable
p/x variable
p/d $r7
ptype gateway_app_upstream_context_t
x/s message.data
x/8i $pc
info registers
info sharedlibrary
~~~

注意有些示例适用于指定业务上下文，**不应一口气运行全部**；尤其 `step` / `continue` 会改变执行位置，变量要在有效的线程与栈帧内读取。输出 `$1`、`$2` 是 GDB 的历史表达式结果编号，不是变量名或线程编号。

### 清理调试会话

调试结束，可以在 GDB 中使用：

~~~gdb
detach
quit
~~~

`detach` 是解除调试附加、允许程序继续运行；如果要**终止**当前由 gdbserver 启动的实验 Gateway，建议确认业务可以停止后在 GDB 执行 `kill`（会终止被调试进程，需考虑是否有输出缓存、队列及资源正常释放要求），或先通过程序已有信号/停止流程完成优雅退出，再退出 GDB。不同 gdbserver 模式下远程调试会话退出的行为可能不同，不能假设 `quit` 总会自动释放 7000 端口。

结束后板端用：

~~~sh
ps | grep smartgateway
ss -lnt 2>/dev/null | grep -E '(:2345|:7000)' || true
~~~

确认调试端口和程序进程是否仍在；根据实际情况停止残留测试实例。**不要仅因 GDB 退出就断言板端所有进程已被清理。**

## 11. 故障现象 → 可能原因 → 处理办法

| 现象 | 可能原因 | 首选处理 |
| --- | --- | --- |
| `Permission denied`（SCP） | 连到错误 IP/账号、SSH 不允许 root、密码错误 | 两侧 `ip -br -4 addr`；核对 SSH 用户及设备 |
| `target remote` 连接拒绝 | 板端 gdbserver 未监听、IP/端口错误、防火墙 | 板端确认 `gdbserver` 日志、`ss -lnt`、连通性 |
| `set sysroot` 带入其他命令 | 多行粘贴合并 | 分两条单独输入，`set sysroot target:` / `target remote ...` |
| `Reading ... from remote target` | 读取共享库 | 检查板端仍连通、gdbserver 未退出；先不要认定崩溃 |
| `No symbol "user_data"` | 当前不在 WiFi Worker 对应栈帧 | `thread 2` / `frame 0` |
| `<optimized out>` | `-Og` 优化或当前指令处变量无位置 | 先 `next` 到变量初始化后；必要时单独建 `-O0 -g3` 版本 |
| `corrupt stack?` / `??` | libc 等无调试符号或无法栈展开 | `info sharedlibrary`、`info registers`、`x/8i $pc`；不要直接断言栈损坏 |
| `pending_message` 找不到 | 源码真实拼写是 `pending_messsage` | `list` 查看当前源代码，输入真实变量名 |
| `P` 未定义 | GDB 命令区分大小写 | 改用小写 `p` |
| `context->tcp` 无此成员 | 成员名实际 `tcp_fd` | `ptype gateway_context_t` 或源码核对 |
| 断点一直不触发 | Agent 没连接或消息未达到此路径；断点禁用 | 先检查 Gateway/Agent 日志、`info breakpoints` |
| 停在断点后 Sensor Agent 暂停 | GDB all-stop 暂停多个 Gateway 线程 | 继续运行后再判断业务超时 |
| TCP send 成功但 Server 没数据 | 应用层接收/解析未证实 | Server 端独立核对 `seq`、节点 ID、时间戳 |
| 断点行号与函数起始行不同 | 源码行不都对应机器指令 | `info line`、`disassemble /m`，理解 DWARF 行表 |
| 程序运行时突然崩溃 | 二进制/调试符号不匹配、真实 bug 等 | 先保存 `bt full`、`info registers`、`info threads`；不要先认定原因 |

## 12. 需要真正掌握的五个 BSP/嵌入式 Linux 概念

1. **交叉调试的主从关系**：本地 GDB 拥有符号，板端 gdbserver 控制真实 ARM 进程；是 GDB 远程调试用户态，而不是远程运行 x86 可执行文件。
2. **DWARF 与机器指令**：`-g` 辅助源码级调试，编译优化会影响可见变量和栈回溯。
3. **ARM EABI 和系统调用**：`svc` + `r7=168` 可识别 `poll`；`r0` 指向 `pollfd`，`r1=1`、`r2` 是超时。
4. **回调与上下文**：函数名作为回调传地址，`void *user_data` 传业务上下文，实际类型/生命周期需要业务层保证。
5. **生产者—消费者**：WiFi Worker 收数据并入队，Server Worker 独占北向 TCP 连接、出队发送、处理重连；注意队列满、背压、网络断开、重复投递及 shutdown 时的资源生命周期。

### 复现实验验收表

- [ ] 本地 ELF 为 ARM 32 位且包含 `.debug_info/.debug_line`。
- [ ] SCP/ADB 将**同一个版本** ELF 部署到板端。
- [ ] GDB 成功读取 ARM 寄存器并命中 `gateway_app_on_wifi_line()`。
- [ ] `bt` 正确解释 `wifi_client_worker → line_parser_feed → gateway_app_on_wifi_line`。
- [ ] 能由 `r7=168` 推断 `poll`，并读取 `struct pollfd`。
- [ ] 能解释 `bound_node_id` 从空字符串到 `NODE04` 的过程。
- [ ] Server Worker 获取 `queue_result=0` 的 93 字节 NODE04 SAMPLE。
- [ ] `tcp_client_send_all()` 在 Gateway 端走成功路径。
- [ ] （可选进一步验收）对应 `seq` 在 Ubuntu Server NodeStore 中确认收到。
- [ ] 退出调试后没有误留占用 7000/2345 端口的实验进程。

本教程根据 2026-10-23 的真实实验编写；针对其他开发板或本地改动后的源码，断点行号、IP、文件名和变量名需要以实际项目为准。
