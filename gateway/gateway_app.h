#ifndef GATEWAY_APP_H
#define GATEWAY_APP_H

#include "frame_parser.h"
#include "serial_port.h"
#include "tcp_client.h"

#include <time.h>

/*
 * =====================================================================
 * 网关应用层
 *
 * 职责：
 *   1. 维护运行上下文（gateway_context_t）；
 *   2. 解析命令行参数（--serial/--baud/--server/--port）；
 *   3. 处理解析出的帧（DATA -> JSON -> TCP上报）；
 *   4. 管理TCP自动重连。
 *
 * 底层I/O（串口读取、主循环）见 gateway_loop.h。
 * =====================================================================
 */

/*
 * 初始化网关上下文。
 *
 * @param context   输出上下文（调用前无需清零）
 * @param server_ip 服务器地址，例如 "127.0.0.1"
 * @param server_port 服务器端口
 */
void gateway_app_init(
    gateway_context_t *context,
    const char *server_ip,
    unsigned short server_port
);

/*
 * 解析命令行参数。
 *
 * 支持：
 *   --serial <device>  串口设备（必填）
 *   --baud <rate>      波特率（可选，默认9600）
 *   --server <ip>      服务器地址（可选，默认127.0.0.1）
 *   --port <port>      服务器端口（可选，默认9000）
 *   --help / -h        打印帮助
 *
 * @param argc,argv  命令行参数
 * @param serial_device 输出：串口设备路径（未提供时为NULL）
 * @param baud_rate  输出：波特率
 * @param context    用于接收 --server/--port 设置
 *
 * @return 0成功；1参数错误/打印帮助后退出
 */
int gateway_app_parse_args(
    int argc,
    char *argv[],
    const char **serial_device,
    int *baud_rate,
    gateway_context_t *context
);

/*
 * 尝试连接TCP服务器（带重试节流）。
 *
 * 未连接时调用；已连接则直接返回。
 * 连接失败时记录 tcp_connect_error，并设置 next_tcp_retry。
 *
 * @param context 网关上下文
 */
void gateway_app_try_tcp_connect(gateway_context_t *context);

/*
 * 处理一帧已通过CRC校验的消息。
 *
 * 当前只处理DATA帧：
 *   1. frame_decode_data() 解码；
 *   2. message_json 生成JSON；
 *   3. TCP发送到服务器（未连接时丢弃并计数）。
 *
 * 该函数可作为 frame_parser_feed() 的回调，
 * 也是 frame_parser_callback_t 签名（user_data为gateway_context_t*）。
 *
 * @param frame     已通过CRC校验的帧
 * @param user_data 必须是 gateway_context_t*，否则不处理
 */
void gateway_app_on_frame(
    const parsed_frame_t *frame,
    void *user_data
);

/*
 * 处理从TCP服务器收到的一行JSON（控制命令）。
 *
 * 流程：
 *   1. message_json_decode_command() 解码JSON -> frame_command_t；
 *   2. frame_build_command() 组帧；
 *   3. serial_port_write_all() 通过串口下发给节点。
 *
 * 该函数可作为 line_parser_feed() 的回调
 * （user_data必须是gateway_context_t*，
 *  serial_fd 由 gateway_loop 在串口连接建立/断开时更新）。
 *
 * @param line      完整JSON行（不含'\n'，已去除结尾的'\r'）
 * @param length    JSON长度
 * @param user_data 必须是 gateway_context_t*
 */
void gateway_app_on_tcp_line(
    const char *line,
    size_t length,
    void *user_data
);

/*
 * 打印退出时的统计信息。
 *
 * @param context 网关上下文
 * @param parser  帧解析器（用于读取parser.stats）
 */
void gateway_app_print_stats(
    const gateway_context_t *context,
    const frame_parser_t *parser
);

/*
 * 关闭TCP连接（如果存在）。
 *
 * @param context 网关上下文
 */
void gateway_app_close(gateway_context_t *context);

#endif
