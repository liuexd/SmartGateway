# Gateway 应用层阅读指南

本文描述当前实现的代码入口、消息路径和资源生命周期。接口的详细参数、
返回值和阻塞行为以对应头文件为准。

## 启动与退出

1. `main.c` 初始化参数和帧解析器、安装信号处理函数，然后调用
   `gateway_loop_run()`；信号处理函数只修改运行标志。
2. `gateway_loop.c` 创建 WiFi 监听 socket、Worker Group、上行队列和
   Device Manager，再启动 Server Link Worker 和可选的 Bluetooth Worker。
   主线程轮询监听 socket，每个已接受的 WiFi 连接交给独立的 Worker。
3. 退出时先关闭监听 socket，再 shutdown 上行队列，唤醒可能阻塞的
   生产者。Bluetooth 和 Server Link 线程通过 join 等待；detached WiFi
   Worker 使用 Group 计数和条件变量等待。所有使用者退出后才销毁共享资源。

## 上行：设备到 Server

- Bluetooth Worker 独占串口，处理打开、重连和 I/O；`frame_parser_feed()`
  调用 `gateway_app_on_frame()`，处理 DATA、ACK 和 NACK 帧。
- WiFi Worker 独占自己的 client socket 和接收缓冲区；`line_parser_feed()`
  保留不完整行，完整 JSON 行交给 `gateway_app_on_wifi_line()`。
- 两种回调都通过 Device Manager 绑定或刷新设备，再将上行 JSON 投递到
  有界 Message Queue。队列满时生产者可能阻塞；入队不要求 Server 已连接。
- Server Link Worker 是运行期间北向 TCP socket 的唯一拥有者。它非阻塞地
  取队列消息，并在发送与接收 Server 命令之间轮询；发送失败时保留当前
  pending 消息，重连后重试。

## 下行：Server 到设备

`gateway_app_on_tcp_line()` 解码 CMD，通过 Device Manager 按 node_id
投递到设备命令队列。实际串口或 WiFi socket 写入由各连接的 Worker 完成；
路由失败时 Server Link Worker 可以使用自己拥有的北向连接发送 NACK。

Device Manager 的 mutex 保护设备表及每设备命令队列。连接接管或断开时，
通过 fd 和线程身份校验所有权，避免旧 Worker 修改新连接状态；未发送的
命令由调用者收集并处理。Message Queue 汇集上行消息，Device Manager
负责设备身份与下行命令投递，两者职责不同。

## 应用文件边界

`gateway_app.c` 当前按配置、北向连接、上行消息封装、身份绑定、蓝牙帧、
WiFi 行、下行路由和退出统计排列。相关函数共享内部辅助函数及回调上下文，
当前阶段保留单文件以避免引入新的内部接口；如果这些职责继续增长，应以
蓝牙帧、WiFi 消息和下行路由为候选边界，另行设计内部接口并验证调用关系。

## 可重复验证

在 Linux 主机安装 GCC、Make、Python 3 和 `socat` 后运行：

```sh
make clean
make -j4 gateway server mock_node mock_wifi_node
make test tests-bin
make test-gateway-smoke
```

模拟测试需要 TCP 7000 空闲，使用虚拟串口、Python WiFi 客户端和模拟
Server，自动检查上下行、重连及退出。它不替代 i.MX6ULL 和真实设备验收。
