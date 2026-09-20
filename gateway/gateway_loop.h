#ifndef GATEWAY_LOOP_H
#define GATEWAY_LOOP_H

#include "frame_parser.h"
#include "serial_port.h"

/*
 * =====================================================================
 * 网关主循环（WiFi 接入分发器）
 *
 * 职责：
 *   1. 创建并启动各 worker 线程与共享资源（上行消息队列）；
 *   2. 循环 poll WiFi 监听 socket，为新接入的节点派发 worker 线程；
 *   3. 退出时按序唤醒、等待并销毁所有线程与资源。
 *
 * 具体的 I/O 都已下放到各自的 worker 线程：
 *   - 串口 I/O     -> bluetooth_worker   （见 bluetooth_worker.h）
 *   - 北向 TCP     -> server_link_worker （见 server_link.h）
 *   - WiFi 客户端  -> wifi_client_worker （见 wifi_client.h）
 *
 * 应用层逻辑（参数解析/JSON 编解码/命令下发）见 gateway_app.h。
 * =====================================================================
 */

/*
 * 运行网关主循环，直到 running 被置0或发生致命错误。
 *
 * @param serial_device 串口设备路径，例如 "/tmp/ttyGW"
 * @param baud_rate     波特率，例如 9600
 * @param context       网关上下文（用于统计，需已初始化）
 * @param parser        帧解析器（需已调用 frame_parser_init）
 * @param running       运行标志；被信号处理函数置0时退出
 *
 * @return 0：正常退出（running被清0）
 */
int gateway_loop_run(
    const char *serial_device,
    int baud_rate,
    gateway_context_t *context,
    frame_parser_t *parser,
    const volatile int *running
);

#endif
