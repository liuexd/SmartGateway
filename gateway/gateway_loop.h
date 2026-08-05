#ifndef GATEWAY_LOOP_H
#define GATEWAY_LOOP_H

#include "frame_parser.h"
#include "serial_port.h"

/*
 * =====================================================================
 * 网关主循环（串口读写）
 *
 * 职责：
 *   1. 打开串口（失败则每秒重试）；
 *   2. 外层循环：串口断线重连；
 *   3. 内层循环：等待可读 -> 读取 -> 喂帧解析器；
 *   4. 通过回调通知应用层（gateway_app_on_frame）。
 *
 * 应用层逻辑（参数解析/TCP上报）见 gateway_app.h。
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
