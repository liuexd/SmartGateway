# D09 · ARM GDB 远程调试与 perf 性能分析实机验收

> 实测日期：2026-10-20（以开发者当日终端输出为准）  
> 开发分支：`feature/bsp-imx6ull`  
> 仓库：`liuexd/SmartGateway`  
> 实验平台：100ASK i.MX6ULL / ARM Cortex-A7 / Linux 4.9.88 / Buildroot  
> 验收边界：下述 PASS 仅针对提供的实机命令输出及观察窗口；未执行的压力、长期稳定性或问题根因分析不得据此判定 PASS。

## 1. 目标及验收结论

本阶段验证交叉编译用户态程序、GDB 远程调试、实际故障定位、`perf stat` 进程统计、`perf record/report` CPU 采样、ELF 符号/DWARF 行号定位，以及多节点通信负载下的性能指标。

**结论：D09 GDB/perf 核心工具实践 PASS；Gateway 长期运行稳定性与 perf 用户态自动符号化仍未完成。** 详见第 8 节。

## 2. 测试环境与程序

| 项目 | 实测或本阶段使用情况 |
| --- | --- |
| Ubuntu 工作区 | `~/Desktop/project/SmartGateway-BSP` |
| 开发板 IPv4 | `192.168.10.3` |
| Ubuntu Server / 模拟节点 IPv4 | `192.168.10.2` |
| Gateway 南向监听 | TCP `7000` |
| 上游 Server | TCP `9000` |
| Ubuntu GDB | `/usr/bin/gdb-multiarch`，17.1 |
| 板端 gdbserver / 原生 GDB | 8.2.1 |
| 板端 perf | 软件事件 `task-clock`、`context-switches`、`page-faults` 和 `cpu-clock` 已实测可用 |
| GPIO Agent 调试程序 | Ubuntu `build/arm/gpio_agent_dbg`；板端 `/tmp/gpio_agent_dbg` |
| Gateway | Ubuntu `build/arm/smart_gateway`；板端 `/tmp/smart_gateway` |
| 新 perf 调试构建 | Ubuntu `build/arm-perf/smart_gateway`，未在该轮重新部署或采样 |

板端 GPIO D08 实验设备树已安全回滚为原有启动环境。本轮 GDB 使用缺少 `/dev/sgw_event0` 的环境作为预期故障用例，不视为驱动回归缺陷。本轮没有改写 eMMC 启动文件、U-Boot 环境、内核或 DTB。

## 3. GDB：远程连接与单步调试

### 3.1 调试器与连接问题

初次尝试远程调试时，曾观察到以下异常：

- GDB 17.1 `next` 触发内部断言 `finish_step_over: Assertion ... trap_expected failed`；这属于调试器内部错误，**不等价于被调试程序崩溃**。
- 远程目标 XML 出现 `unknown architecture "arm"`、`Remote 'g' packet reply is too long`；当时 `show architecture` 显示 `auto (currently "i386")`，`set architecture arm` 显示 `Undefined item: "arm"`。
- 以太网 `target remote 192.168.10.3:2345` 曾超时，期间有 gdbserver 未监听的情况；不能由超时单独推断 ADB 或网口故障。

之后通过明确启动 ARM 多架构调试器 `/usr/bin/gdb-multiarch`，并在板端运行 gdbserver、使用网口 `192.168.10.3:2345` 连接，成功命中断点。板端原生 GDB 的 `run`、`next` 也已单独验证有效。

### 3.2 远程断点、单步、变量、栈：PASS

```gdb
(gdb) target remote 192.168.10.3:2345
(gdb) break main
(gdb) continue
Breakpoint 1, main () at main.c:249
249         int gpio_fd = -1, tcp_fd = -1;
(gdb) next
250         int exit_code = EXIT_FAILURE;
(gdb) print gpio_fd
$1 = -1
(gdb) print tcp_fd
$2 = -1
(gdb) bt
#0  main () at main.c:250
```

曾重复设置两个 `main` 断点；后以 `delete 2` 清理重复断点。此后远程 `next` 正常，没有再次观察到此前的内部断言。

## 4. GDB：`open()` 失败原因定位：PASS

在 `bsp/apps/gpio_agent/main.c` 的 `open(GPIO_DEVICE, O_RDONLY | O_NONBLOCK)` 之后设置一次性断点：

```gdb
(gdb) tbreak main.c:264
(gdb) continue
Temporary breakpoint 3, main () at main.c:264
264         if (gpio_fd < 0) {
(gdb) print gpio_fd
$3 = -1
(gdb) print *(int *)__errno_location()
$5 = 2
(gdb) next
265             perror("open GPIO device");
(gdb) next
266             return EXIT_FAILURE;
(gdb) continue
[Inferior 1 (process 14296) exited with code 01]
```

`errno = 2` 对应 `ENOENT`：目标文件或路径不存在。开发板交叉验证：

```text
ls -l /dev/sgw_event0
ls: cannot access '/dev/sgw_event0': No such file or directory

ls -ld /proc/device-tree/sgw-gpio-event
ls: cannot access '/proc/device-tree/sgw-gpio-event': No such file or directory

cat /proc/modules | grep sgw_gpio_event
# 无输出
```

**结论：** 该轮 `/dev/sgw_event0` 不存在、D08 设备树节点不存在、`sgw_gpio_event` 模块未加载；Agent `open()` 因 `ENOENT` 失败，随后返回 `EXIT_FAILURE=1`。这是可解释的启动/驱动环境缺失，而不是通过本次证据发现了新的 GPIO 驱动 bug。

注意：`print errno` 在此调试环境提示 `__errno_location@plt has unknown return type`，改为 `print *(int *)__errno_location()` 后成功。此调用写法针对当前板端 libc/GDB 环境。

## 5. perf stat：软件事件与网关负载统计

### 5.1 板端事件采集能力：PASS

板端实际执行：

```sh
perf stat -e task-clock,context-switches,page-faults -- sleep 3
```

结果：

```text
4.198667   task-clock (msec)
2          context-switches
35         page-faults
3.007188667 seconds time elapsed
```

本结果证明板端 perf 可以统计上述软件事件。Ubuntu 虚拟机曾因 `perf_event_paranoid=4` 无权限采集，该结果不代表开发板 perf 不可用。

### 5.2 Gateway 三档负载测试

板端运行 `/tmp/smart_gateway --no-serial --server 192.168.10.2 --port 9000`；Ubuntu 上运行 Server 与模拟 Wi-Fi 节点，使用：

```sh
PID=$(pidof smart_gateway)
perf stat -e task-clock,context-switches,page-faults -p "$PID" -- sleep 10
```

| 场景 | task-clock | context-switches | page-faults | elapsed | 约平均单核 CPU 利用率 |
| --- | ---: | ---: | ---: | ---: | ---: |
| Gateway 空闲基线，单次 | 10.521998 ms | 110 | 0 | 10.017191334 s | 0.105% |
| NODE04 单节点，单次 | 26.466332 ms | 210 | 0 | 10.004862335 s | 0.265% |
| 五节点负载，`-r 3` 三轮平均 | 58.934995 ms | 610 | 0 | 10.007855557 s/轮 | 0.589% |

五节点重复测量命令：

```sh
perf stat -r 3 -e task-clock,context-switches,page-faults -p "$PID" -- sleep 10
```

三轮报告显示 `task-clock` 波动约 `±0.78%`，`context-switches` 波动约 `±0.09%`，墙钟时间波动约 `±0.01%`。此前还有一次 `64.507000 ms / 609 switches` 的单次采样，但相关输出与节点启动命令的先后顺序不足以独立确认负载条件，因此**不将该次数据作为正式的五节点基线**。

**解读边界：** CPU 利用率按 `task-clock / elapsed` 计算，属于所测进程在观察窗口的平均单核等效 CPU 利用率，并非整机 CPU 利用率。上下文切换增加与多连接处理相符，但不能仅凭计数断言具体线程导致额外开销。空闲和单节点各只有一轮，不支持严谨的性能伸缩性结论。本实验不是极限吞吐或长期压力测试。

### 5.3 五节点消息链路：PASS（提供日志窗口）

Ubuntu 运行 NODE04～NODE08 五个独立 `mock_wifi_node` 进程，并实测与 `192.168.10.3:7000` 建立了五条 TCP `ESTAB` 连接。Server `Node registry` 显示五节点均 `ONLINE`。在同一份所提供日志中，每个节点 `seq=93` 至 `97` 的五条记录均出现 `[Message]` 与 `[NodeStore]`，合计 **25 条可见已处理样本**。

通信路径：

```text
Ubuntu mock_wifi_node × 5
  -> 192.168.10.3:7000（i.MX6ULL Gateway）
  -> 192.168.10.2:9000（Ubuntu Server）
  -> NodeStore
```

`dev=UNKNOWN` 表示当前 `LIGHT_RAW` 测试消息未被分类为特定设备类型，但不影响本次消息链路到达验证。仅证明观察窗口，不推断长期在线或端到端永不丢包。

## 6. perf record/report 与 ELF 符号分析

### 6.1 采样：PASS

开发板 `/tmp` 中执行：

```sh
perf record -e cpu-clock -p $(pidof smart_gateway) -- sleep 30
perf report --stdio -i /tmp/perf.data
```

实际输出：

```text
[ perf record: Woken up 1 times to write data ]
[ perf record: Captured and wrote 0.016 MB perf.data (242 samples) ]

# Total Lost Samples: 0
# Samples: 242 of event 'cpu-clock'
# Event count (approx.): 60500000
```

报告中可见的内核采样符号（各占该份报告采样权重）：

| 符号 | Overhead |
| --- | ---: |
| `_raw_spin_unlock_irq` | 8.68% |
| `__softirqentry_text_start` | 3.31% |
| `_raw_spin_unlock_irqrestore` | 2.48% |
| `tcp_sendmsg` | 2.48% |
| `n_tty_write` | 1.65% |

这些数值是有限样本的采样占比，**并非精确函数执行时间，也不足以断言锁竞争、网络协议栈或终端输出已构成性能瓶颈**。

### 6.2 原始 ELF 符号及 Build ID：PASS

原始 Ubuntu `build/arm/smart_gateway`：ARM 32-bit ELF，带 `debug_info`，未剥离符号；`readelf -Ws` 可找到 `wifi_client_worker`、`server_link_worker` 和 `gateway_loop_run`。

Ubuntu 构建文件和板端 `/tmp/smart_gateway` 的 SHA-256 完全相同：

```text
9fe1999c0535bb7eed8999ecd7d1c326cedca9f026a130acaab851e226ac38bf
```

`perf buildid-list -i /tmp/perf.data` 显示：

```text
82f1f93b0cb7060f81e5f14e9e8ed812f58e98ac /tmp/smart_gateway
```

该 Build ID 与 Ubuntu 原始 ELF 一致。

但是原始 `perf report` 的部分 Gateway 用户态符号显示为 `[.] 0x00004724`、`[.] 0x00007b20`、`[.] 0x000090d4`。以 `+0x10000` 的候选地址偏移调用原始 ELF 的 `addr2line -f -C`，分别得到 `message_json_decode_data`、`wifi_client_worker`、`device_manager_find_index_locked`。这是函数级的合理映射线索；**该偏移关系尚未通过完整的 ELF 映射元数据独立证明，不能作为通用转换公式**。

### 6.3 DWARF 缺少业务源码行号与独立调试构建：PASS

原始 ELF 的调试段大小：

| ELF | `.debug_info` | `.debug_line` |
| --- | ---: | ---: |
| `build/arm/smart_gateway` | 1,179 B | 630 B |
| `build/arm-perf/smart_gateway` | 63,311 B | 32,701 B |

原始 `readelf --debug-dump=decodedline` 显示的主要为启动/运行时文件 `start.S`、`crti.S`、`elf-init.c`、`crtn.S`，没有观察到 Gateway 业务源码行号。仓库根 `makefile` 默认 `CFLAGS` 不包含 `-g`，这与此前部分对象文件没有完整 DWARF 行号信息的现象相符。

为避免覆盖已验证的原始 Gateway，使用独立输出目录强制重新编译：

```sh
make -B gateway \
    BUILD_DIR=build/arm-perf \
    CC=arm-linux-gnueabihf-gcc \
    CFLAGS="-O2 -g3 -fno-omit-frame-pointer -Wall -Wextra -Werror -std=c11 -pthread"
```

新的 `readelf --debug-dump=decodedline` 已显示 `message_json.c` 多个行号与代码地址；`addr2line` 实测：

```sh
arm-linux-gnueabihf-addr2line -f -C \
    -e build/arm-perf/smart_gateway 0x1226c
```

```text
kv_array_add
/home/ubuntu/Desktop/project/SmartGateway-BSP/common/message_json.c:410
```

**结论：新 ELF 的 DWARF 源码行号解析 PASS。** 该 `0x1226c` 来源于新 ELF 的 `.debug_line`，不是从旧版 perf 样本地址直接换算得来。新调试构建**尚未重新部署并采样**，因此不得声称旧 `perf.data` 已能直接在板端报告中自动显示新 ELF 的业务函数名。

## 7. 常见问题与操作注意事项

- `perf stat -p PID -- sleep 10` 测量的是指定 Gateway 进程，`sleep` 只是控制统计窗口；不要将其解释为测量 `sleep` 的 CPU 用量。
- `perf record -e cpu-clock` 实际采样数受目标进程 CPU 活动影响；`-F` 设定采样频率也不保证每秒收到同等数量样本。
- `perf report` 的 `[k]` 为内核符号，`[.]` 为用户态；符号解析依赖匹配的 ELF/Build ID、映射与工具功能。
- 开发板的 `perf` 可用，不代表 Ubuntu VM 普通用户的 `perf` 也可用；Ubuntu 曾出现 `perf_event_paranoid=4` 权限限制。
- 多行 Shell 命令中的 `>` 是续行提示符；为减少终端粘贴问题，实测成功的采样使用单行命令。
- `perf buildid-list` 可能进入分页器，按 `q` 退出；终端显示异常时可用 `stty sane`、`reset` 恢复。
- 模拟节点必须从正确工作目录启动，或者用绝对路径。曾在 Ubuntu 家目录执行 `./build/mock_wifi_node` 导致 `Exit 127`，后切换仓库根目录恢复正常。
- `mock_wifi_node` 的独立后台任务曾结束，并打印 `Closing connection ... / mock_wifi_node stop`；这不证明 Gateway 崩溃。需要另外捕获退出码与时序。

## 8. 未完成事项及风险

1. **Gateway 曾出现意外退出或执行会话终止的现象；根因未查明。** 尚未获得可证明的 Gateway 退出码、崩溃栈或内核 OOM/信号证据；应在后续稳定性测试中捕获退出状态、完整日志与进程状态，区分进程退出与 ADB/终端会话结束。
2. **新调试 ELF 的 perf 用户态自动符号化未实测。** 新 `build/arm-perf/smart_gateway` 的 `addr2line` 已验证到源码行，但未部署、未重新 `perf record/report`。
3. **性能数据范围有限。** 空闲和单节点场景各一轮、五节点三轮；数据量小、模拟节点发送频率约每 2 秒一条，不能据此证明容量极限、锁竞争瓶颈或长期性能稳定性。
4. **D08 字符设备未恢复。** GPIO Agent `ENOENT` 只用作负向调试案例；本轮未测 GPIO 真实事件端到端性能。

## 9. 最终验收矩阵

| 编号 | 项目 | 结果 | 核心证据 |
| --- | --- | --- | --- |
| GDB-01 | ARM 远程连接与 `main` 断点 | **PASS** | 命中 `main.c:249` |
| GDB-02 | 单步、变量、调用栈 | **PASS** | `next` 至 250、变量 -1、`bt` |
| GDB-03 | `open()` / `errno` 故障定位 | **PASS** | `gpio_fd=-1`、`errno=2`、退出码 1、设备缺失 |
| PERF-01 | 板端 perf 软件事件支持 | **PASS** | `sleep 3` 的三项事件 |
| PERF-02 | Gateway 空闲/单节点基线 | **PASS（单次）** | 10.522/26.466 ms CPU 时间 |
| PERF-03 | 五节点统计 | **PASS（三轮）** | 58.935 ms、610 次、±0.78% |
| NET-01 | 五节点同时在线并被 NodeStore 处理 | **PASS（观察窗口）** | 五条 `ESTAB`，五节点 seq 93–97 |
| PERF-04 | CPU 采样与报告 | **PASS** | 242 samples、lost=0、内核符号 |
| ELF-01 | 原始 Gateway ELF / Build ID 一致 | **PASS** | SHA-256、Build ID 一致 |
| ELF-02 | 独立调试 ELF 源码行号 | **PASS** | `kv_array_add` → `message_json.c:410` |
| PERF-05 | 新调试 ELF 在板端自动用户态符号化 | **NOT TESTED** | 未重新部署/采样 |
| REL-01 | Gateway 意外退出根因 | **UNRESOLVED** | 缺少退出码/栈/内核证据 |

**阶段收尾原则：** 本文仅归档可见实机证据，不将工具使用成功外推为长期稳定性、极限性能或缺陷根因已修复。