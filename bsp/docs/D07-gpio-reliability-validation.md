# D07 · GPIO 驱动异常与可靠性验证

> 验收整理日期：2026-10-15（北京时间）
>
> 分支：`feature/bsp-imx6ull`
>
> 基础功能验收：[D06 · GPIO 事件到 Server 端到端联调](D06-gpio-end-to-end-validation.md)
>
> 相关实现基线：[80e4631](https://github.com/liuexd/SmartGateway/commit/80e4631be88f5793bccc35c09e084305c4c05f6f)
>
> **总体结论**：本轮已提供日志的功能与恢复场景通过；REL-05、REL-07 依据开发者反馈此前已验收，本轮跳过且未提供新增原始记录。此文不宣称零丢包、Exactly-Once、已测量消抖阈值或长期无泄漏。

## 1. 范围与证据规则

测试环境沿用 D06：100ASK i.MX6ULL、Linux 4.9.88 / Buildroot（版本参考 D01/D02 已保存基线，**本轮未重采集**）、`sgw_gpio_event.ko`、`/dev/sgw_event0`、`/tmp/gpio_agent`、`/tmp/smart_gateway` 与 Ubuntu `192.168.10.2:9000` 上的 `gateway_server`。

本文件只记录开发者在实际开发板与 Ubuntu 终端中提供的输出及其能够支持的结论：

- **PASS（本轮日志）**：输出支持相应结论。
- **PASS（限定范围）**：仅对当次功能行为成立；不外推为所有边界情形都可靠。
- **PRIOR PASS（用户确认）**：此前完成并由开发者确认，本轮未重跑且当前会话未提供原始数据；如需第三方审阅，仍应补对应历史日志或复验记录。
- **NOT VERIFIED**：尚缺足够的实测证据，不能视为完成。

## 2. 验收总表

| 编号 | 项目 | 本次归档状态 | 核心证据或说明 |
| --- | --- | --- | --- |
| REL-01 | 空队列非阻塞 `read` / `EAGAIN` | **PASS** | 板端 `PASS: empty queue returns EAGAIN`，`exit_code=0` |
| REL-02 | `poll` 等待、GPIO 唤醒与事件读取 | **PASS（功能）** | seq=9/10；`enqueued` 8→10，`depth=0` |
| REL-03A | Server 停止后自动重试与恢复连接 | **PASS** | 多次连接失败后 `connection = 2`，`sent, bytes=143` |
| REL-03B | Server 重启后的事件接收与顺序 | **PASS（本轮样本）** | Server NodeStore 接收 `EVT_SEQ=11/12` |
| REL-03C | **严格验证离线产生消息的无丢失/重传语义** | **NOT VERIFIED** | 缺按键时刻/Agent 发送时刻与 Server 离线窗口对应证据；不保证应用层确认 |
| REL-04A | 正常速度 5 次按下/释放 | **PASS（驱动层）** | `enqueued` 12→22，`dropped=0` |
| REL-04B | 快速按下/释放约 10 次 | **PASS（驱动层）** | `enqueued` 22→42，`dropped=0`；未测精确 20 ms 抑制阈值 |
| REL-05 | 64 项环形队列溢出 | **PRIOR PASS（用户确认；本轮跳过）** | 历史验收日志未在本轮提供；不能据此附加新的计数或结论 |
| REL-06 | 正常卸载、重新加载与业务恢复 | **PASS** | `rmmod/insmod` 均为 0，设备注销/重建，Server 收到 `EVT_SEQ=1/2` |
| REL-07 | 30 分钟连续运行 | **PRIOR PASS（用户确认；本轮跳过）** | 开发者明确已跑满 30 分钟并验收成功；本轮未提供 RSS/线程/心跳采样记录 |

## 3. REL-01：非阻塞读取 EAGAIN

交叉编译产物检查：

~~~text
/tmp/sgw_nonblock_test: ELF 32-bit LSB executable, ARM, EABI5 version 1 (SYSV), dynamically linked, interpreter /lib/ld-linux-armhf.so.3, for GNU/Linux 2.6.32, ... with debug_info, not stripped
~~~

ADB 传输：

~~~text
/tmp/sgw_nonblock_test: 1 file pushed, 0 skipped. ... (10784 bytes in 0.001s)
~~~

板端输出：

~~~text
sh-5.0# /tmp/sgw_nonblock_test
PASS: empty queue returns EAGAIN
sh-5.0# echo "exit_code=$?"
exit_code=0
~~~

**判定**：非阻塞模式下空队列读取符合预期 `read()=-1` 且 `errno=EAGAIN`。测试源文件：`bsp/apps/sgw_event_test/nonblock_test.c`。

## 4. REL-02：poll 等待 / 事件通知

受控复测中，`/tmp/sgw_poll_test` 输出：

~~~text
Listening on /dev/sgw_event0
Waiting for GPIO interrupts...
seq=9 level=0 state=PRESSED time_ms=71080780
seq=10 level=1 state=RELEASED time_ms=71080980
~~~

测试前已记录 `irq_total=12, enqueued=8, depth=0, last_sequence=8`；测试后：

~~~text
irq_total=19
enqueued=10
dropped=0
depth=0
capacity=64
head=10
tail=10
last_sequence=10
last_level=1
last_timestamp_ns=71080980159140
~~~

**判定**：新产生的 seq 9/10 可由 `poll/read` 取得并被消费。证明基本可读通知链路正常；没有量化中断至唤醒的精确耗时。

## 5. REL-03：Server 停止、自动重连、恢复转发

### 5.1 断连恢复

Gateway 侧反复出现：

~~~text
[TCP] connecting to 192.168.10.2:9000...
[TCP] tcp_connect failed: 192.168.10.2:9000
[TCP] connecting to 192.168.10.2:9000...
[TCP] connected to 192.168.10.2:9000, connection = 2
[SERVER WORKER] sent, bytes=143
~~~

说明 Server 恢复后，Gateway 能建立新连接并继续发送上行数据。`sent` 是发送接口结果，不单独证明 Server 应用层消费。

### 5.2 Server 重启后收到 GPIO 事件

Server 侧：

~~~text
^CTCP server stopped.
$ ./build/gateway_server 9000
TCP server listening on 127.0.0.1:9000
[TCP] client connected: 192.168.10.3:57618
[NodeStore] node=NODE03 seq=186 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=HEARTBEAT,STATUS=ALIVE,LEVEL=1,STATE=RELEASED
[NodeStore] node=NODE03 seq=187 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=EDGE,LEVEL=0,STATE=PRESSED,EVT_SEQ=11,TS_NS=80574340274271
[NodeStore] node=NODE03 seq=188 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=EDGE,LEVEL=1,STATE=RELEASED,EVT_SEQ=12,TS_NS=80574540236605
[NodeStore] node=NODE03 seq=189 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=HEARTBEAT,STATUS=ALIVE,LEVEL=1,STATE=RELEASED
~~~

板端统计从：

~~~text
irq_total=19
enqueued=10
dropped=0
depth=0
last_sequence=10
~~~

到：

~~~text
irq_total=23
enqueued=12
dropped=0
depth=0
head=12
tail=12
last_sequence=12
last_level=1
last_timestamp_ns=80574540236605
~~~

**判定**：Server 重启、TCP 恢复后，按下/释放事件和心跳确实进入 NodeStore。**无法仅凭这组日志证实事件必然发生于 Server 离线窗口内**，也无法证明断网期间所有待发送消息无丢失、无重复。建议日后单独增加对齐时间的 Agent/Server 日志与序号对照。

## 6. REL-04：正常速度与快速按键

| 快照 | irq_total | enqueued | dropped | depth | head | tail | last_sequence | last_level |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 正常按键前 | 23 | 12 | 0 | 0 | 12 | 12 | 12 | 1 |
| 5 次按下/释放后 | 38 | 22 | 0 | 0 | 22 | 22 | 22 | 1 |
| 快速按键后 | 58 | 42 | 0 | 0 | 42 | 42 | 42 | 1 |

- REL-04A：5 次正常按下/释放，`enqueued +10`，符合 10 次稳定状态变化的预期；`irq_total +15`。
- REL-04B：约 10 次快速按下/释放，`enqueued +20`，`irq_total +20`，无队列满丢弃，最终电平保持释放态。
- `irq_total` 与 `enqueued` 的差异**不能直接算成“消抖成功过滤次数”**；准确验证 20ms 阈值需要受控电平刺激与时间测量。
- 本轮 REL-04 的证据为驱动 stats；**未保存逐条对应的 Server NodeStore 日志**，因此本节不宣称全链路 10/20 条零丢失。

## 7. REL-05：环形队列溢出（已在此前验收）

开发者说明：此测试此前已进行并通过，本轮明确要求跳过。

**记录状态：PRIOR PASS（用户确认，本轮未重新执行）**。建议有空时将历史环形队列满、`dropped` 增长、消费者读取及队列恢复的原始证据补到独立附件。不生成或推测历史计数。

## 8. REL-06：模块资源生命周期与业务恢复

卸载前：

~~~text
sgw_gpio_event 5107 0 - Live 0x7f016000 (O)
crw------- 1 root root 10, 56 Jan  6 23:16 /dev/sgw_event0
irq_total=58
enqueued=42
dropped=0
depth=0
head=42
tail=42
~~~

停止 Agent 和测试程序，保留 `/tmp/sgw_gpio_event.ko`，随后：

~~~text
sh-5.0# rmmod sgw_gpio_event
sh-5.0# echo "rmmod_exit=$?"
rmmod_exit=0
sh-5.0# cat /proc/modules | grep sgw_gpio_event
sh-5.0# ls -l /dev/sgw_event0
ls: cannot access '/dev/sgw_event0': No such file or directory
sh-5.0# ls /sys/devices/platform/sgw-gpio-event
ls: cannot access '/sys/devices/platform/sgw-gpio-event': No such file or directory
~~~

重新加载：

~~~text
sh-5.0# insmod /tmp/sgw_gpio_event.ko
sh-5.0# echo "insmod_exit=$?"
insmod_exit=0
sgw_gpio_event 5107 0 - Live 0x7f026000 (O)
crw------- 1 root root 10, 56 Jan  7 03:21 /dev/sgw_event0
irq_total=0
enqueued=0
dropped=0
depth=0
capacity=64
head=0
tail=0
last_sequence=0
last_level=0
last_timestamp_ns=0
~~~

内核日志：

~~~text
sgw_gpio_event: module init
sgw-gpio-event sgw-gpio-event: probe: start
sgw-gpio-event sgw-gpio-event: initial stable level=1 debounce=20ms
sgw-gpio-event sgw-gpio-event: misc device /dev/sgw_event0 ready
sgw-gpio-event sgw-gpio-event: IRQ 194 ready, queue capacity=64
sgw_gpio_event: module loaded
~~~

重新加载后再次触发真实按键：

~~~text
irq_total=3
enqueued=2
dropped=0
depth=0
capacity=64
head=2
tail=2
last_sequence=2
last_level=1
~~~

Server 对应收到：

~~~text
[NodeStore] node=NODE03 seq=10 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=EDGE,LEVEL=0,STATE=PRESSED,EVT_SEQ=1,TS_NS=81715800137074
[NodeStore] node=NODE03 seq=11 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=EDGE,LEVEL=1,STATE=RELEASED,EVT_SEQ=2,TS_NS=81715990150741
[NodeStore] node=NODE03 seq=12 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=HEARTBEAT,STATUS=ALIVE,LEVEL=1,STATE=RELEASED
[NodeStore] node=NODE03 seq=13 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=HEARTBEAT,STATUS=ALIVE,LEVEL=1,STATE=RELEASED
[NodeStore] node=NODE03 seq=14 dev=GPIO_BUTTON fields=DEV=4,GPIO=115,KIND=HEARTBEAT,STATUS=ALIVE,LEVEL=1,STATE=RELEASED
~~~

**判定：PASS**。卸载后设备接口消失；加载后新 `probe`、IRQ 和 misc 设备注册成功；重新触发 GPIO 后，Server 正确处理新实例的事件序号 1、2。并未独立测量内核内存泄漏，也未测试在读者持有 FD 时强制卸载模块。

备注：`initial stable level=1` 而无事件时 `last_level=0`，是因为 `last_event` 结构体刚被清零，不应将无有效事件时的 `last_level` 当成当前引脚电平。

## 9. REL-07：30 分钟稳定性（已在此前验收）

开发者明确反馈此前已经连续运行 30 分钟并完成验收，因此本轮跳过重复执行。

**记录状态：PRIOR PASS（用户确认）**。本次对话没有提供那一轮 T0/T+10/T+20/T+30 的 RSS、线程数、丢弃计数和日志，因此不能补写具体采样数值、增长率或泄漏排除结论。未来项目面试展示可补历史数据及条件说明。

## 10. 剩余风险和后续阶段

1. **应用层可靠性边界**：Agent 的 `send()` 成功不是 Server 的持久化 ACK；长时间断线、队列容量限制、重连期间重复发送与幂等处理需另外验收。
2. **并发边界**：本轮正常卸载前停止了所有设备读者，不能替代持有 FD、阻塞 `read/poll`、不同线程同时退出的专项测试。
3. **消抖时序**：20ms 滤抖机制已经集成，但未使用可控波形对时间阈值做定量测量。
4. **驱动设计**：当前仍硬编码 `SGW_GPIO=115`，并通过 `platform_device_register_simple()` 创建设备，不属于 DT 自动绑定。
5. **设备恢复前置条件**：D01 厂商镜像、硬件匹配和安全恢复路径仍未验收；**在此之前不得直接覆盖 eMMC 上现用的 U-Boot / Kernel / DTB**。

**下一阶段 D08：GPIO Device Tree 资源化。** 首先只读检查原厂 DTS / live Device Tree 的 GPIO4_IO19 引脚复用与占用，再创建离线 DTS/DTB 和 OF 驱动实验；只有在确认安全恢复与备用启动条件后才安排实机定制 DTB 启动。

## 11. 阶段结论

**D07 本轮核心测试完成，阶段记录归档。** REL-01、REL-02、REL-03、REL-04、REL-06 均有开发者实测输出支持的功能结果；REL-05、REL-07 均为开发者确认的历史验收项目，本轮跳过、无新增原始日志。不能据此宣称项目已通过完整零丢包保证、超长压力测试或全部内核并发边界测试。
