#ifndef FRAME_PARSER_H
#define FRAME_PARSER_H

#include "frame.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * =====================================================================
 * 帧解析（接收方向）
 *
 * 本头文件负责“拆帧”：
 *   1. frame_parser_feed() 逐字节喂入原始数据流，提取完整帧并校验CRC；
 *   2. 校验通过后通过回调传出 parsed_frame_t；
 *   3. 用 frame_get_message_type() 判断帧类型；
 *   4. 根据类型调用对应的 frame_decode_*() 解析为结构体。
 *
 * 对应的“组帧”（发送方向）见 frame.h 中的 frame_build_*() 系列函数。
 * =====================================================================
 */

/*
 * 一帧经过CRC校验后的通用结果。
 *
 * raw：
 * @1,NODE01,000001,DATA,T=253,H=601*3F2E\r\n
 * （完整原始帧，含帧头@、分隔符*、CRC和\r\n）
 *
 * payload：
 * 1,NODE01,000001,DATA,T=253,H=601
 * （参与CRC计算的数据区，不含@、*、CRC和\r\n）
 *
 * received_crc   ：帧中携带的CRC值（帧尾*后面的4位十六进制）
 * calculated_crc ：对payload重新计算得到的CRC值
 *                  （两者相等表示帧在传输中未被破坏）
 */
typedef struct
{
    char raw[FRAME_MAX_LEN];
    size_t raw_length;

    char payload[FRAME_MAX_LEN];
    size_t payload_length;

    uint16_t received_crc;
    uint16_t calculated_crc;
} parsed_frame_t;

/*
 * DATA（温湿度数据）帧解析后的数据。
 *
 * node_id        ：节点编号，例如 "NODE01"
 * sequence       ：帧序号，范围 0~999999
 * temperature_x10：温度放大10倍后的整数，253 表示 25.3℃
 * humidity_x10   ：湿度放大10倍后的整数，601 表示 60.1%
 */
typedef struct
{
    char node_id[32];
    uint32_t sequence;
    int temperature_x10;
    int humidity_x10;
} frame_data_t;

/*
 * 解析器运行统计。
 *
 * valid_frames     ：成功解析并通过CRC校验的帧数
 * crc_errors       ：帧格式基本正确但CRC校验失败的帧数
 * format_errors    ：帧头/帧尾/字段数量等格式错误的帧数
 * overflow_errors  ：帧长度超过 FRAME_MAX_LEN 而被丢弃的帧数
 * discarded_bytes  ：等待帧头时丢弃的无效字节数
 */
typedef struct
{
    unsigned long valid_frames;
    unsigned long crc_errors;
    unsigned long format_errors;
    unsigned long overflow_errors;
    unsigned long discarded_bytes;
} frame_parser_stats_t;

/*
 * 流式解析器内部状态。
 *
 * buffer/length：当前正在接收的一帧的缓冲区及其长度
 * collecting    ：状态机当前所处阶段
 * stats         ：运行统计，可通过 frame_parser_init() 清零
 */
typedef struct
{
    char buffer[FRAME_MAX_LEN];
    size_t length;

    /*
     * collecting == 0：等待帧头@
     * collecting == 1：正在接收一帧
     */
    int collecting;

    frame_parser_stats_t stats;
} frame_parser_t;

/*
 * 回调上下文：解析器使用者（网关/节点）的运行状态与统计。
 *
 * 该结构体原本定义在 gateway/main.c，
 * 现在移入本头文件，让网关和 mock_node 等工具都能共享。
 *
 * tcp_fd              ：与服务器之间的TCP socket，>=0已连接，<0无连接
 * serial_fd           ：与节点之间的串口fd，>=0已连接，<0无连接
 * tcp_send            ：TCP发送消息次数
 * tcp_send_error      ：TCP发送失败次数
 * tcp_dropped         ：TCP未连接时丢弃的数据数量
 * tcp_connections     ：TCP连接成功次数
 * tcp_connect_error   ：TCP连接失败次数
 * server_ip           ：服务器地址
 * server_port         ：服务器端口
 * next_tcp_retry      ：下次TCP重连时间点（time(NULL)）
 * decode_errors       ：协议字段解析失败次数
 * serial_connections  ：串口连接次数
 */
typedef struct
{
    int tcp_fd;
    int serial_fd;

    time_t next_tcp_retry;
    time_t next_serial_retry;
    
    unsigned long tcp_send;
    unsigned long tcp_send_error;
    unsigned long tcp_dropped;
    unsigned long tcp_connections;
    unsigned long tcp_connect_error;

    const char *server_ip;
    unsigned short server_port;

    unsigned long decode_errors;

    unsigned long serial_connections;
} gateway_context_t;

/*
 * 每成功解析出一帧，解析器就调用一次该函数。
 *
 * 使用回调的原因是：
 * 一次read()可能包含两帧甚至更多帧。
 *
 * @param frame    解析出的完整帧（含CRC校验结果）
 * @param user_data 调用 frame_parser_feed() 时透传的用户数据
 */
typedef void (*frame_parser_callback_t)(
    const parsed_frame_t *frame,
    void *user_data
);

/*
 * 初始化解析器，同时清零统计数据。
 *
 * 使用前必须调用一次。
 */
void frame_parser_init(frame_parser_t *parser);

/*
 * 只清除当前未完成帧，不清除统计数据。
 *
 * 通常在流中断、超时等场景下丢弃半帧时使用。
 */
void frame_parser_reset(frame_parser_t *parser);

/*
 * 向解析器喂入任意长度的数据。
 *
 * 数据会被逐字节送入状态机，解析出的合法帧
 * 会通过 callback 回调给调用者。
 *
 * @param parser      解析器实例
 * @param data        原始数据流
 * @param data_length 数据长度
 * @param callback    每成功解析一帧时调用的回调（可为NULL）
 * @param user_data   透传给回调的用户数据
 *
 * @return 本次数据中成功解析出的合法帧数量
 */
size_t frame_parser_feed(
    frame_parser_t *parser,
    const uint8_t *data,
    size_t data_length,
    frame_parser_callback_t callback,
    void *user_data
);

/*
 * 把已经通过CRC校验的DATA帧进一步解析为结构体。
 *
 * 帧格式示例：
 * 1,NODE01,000001,DATA,T=253,H=601
 *
 * @param frame 已经通过CRC校验的帧
 * @param out   解析结果输出（成功时填充，失败时内容不变）
 *
 * @return 成功返回0，失败返回-1
 */
int frame_decode_data(
    const parsed_frame_t *frame,
    frame_data_t *out
);

/*
 * 把已经通过CRC校验的CMD帧进一步解析为结构体。
 *
 * 帧格式示例：
 * 1,GW01,000001,CMD,LED=1
 *
 * @param frame 已经通过CRC校验的帧
 * @param out   解析结果输出（成功时填充，失败时内容不变）
 *
 * @return 成功返回0，失败返回-1
 */
int frame_decode_command(
    const parsed_frame_t *frame,
    frame_command_t *out
);

/*
 * 把已经通过CRC校验的ACK帧进一步解析为结构体。
 *
 * 帧格式示例：
 * 1,NODE01,000001,ACK,LED=1
 *
 * @param frame 已经通过CRC校验的帧
 * @param out   解析结果输出（成功时填充，失败时内容不变）
 *
 * @return 成功返回0，失败返回-1
 */
int frame_decode_ack(
    const parsed_frame_t *frame,
    frame_ack_t *out
);

/*
 * 把已经通过CRC校验的NACK帧进一步解析为结构体。
 *
 * 帧格式示例：
 * 1,NODE01,000001,NACK,ERR=unsupported_cmd
 *
 * @param frame 已经通过CRC校验的帧
 * @param out   解析结果输出（成功时填充，失败时内容不变）
 *
 * @return 成功返回0，失败返回-1
 */
int frame_decode_nack(
    const parsed_frame_t *frame,
    frame_nack_t *out
);

/*
 * 帧类型枚举，用于标识一帧消息的种类。
 *
 * 该枚举与 frame_get_message_type() 的返回值配合使用，
 * 典型的处理流程是：
 *   1. 回调中拿到 parsed_frame_t；
 *   2. 用 frame_get_message_type() 判断帧类型；
 *   3. 根据类型调用对应的 frame_decode_*() 函数解析内容。
 */
typedef enum {
    FRAME_MESSAGE_UNKNOWN = 0, /* 未知类型，无法识别的帧 */
    FRAME_MESSAGE_DATA,        /* 温湿度数据帧（DATA） */
    FRAME_MESSAGE_CMD,         /* 控制命令帧（CMD） */
    FRAME_MESSAGE_ACK,         /* 命令成功应答帧（ACK） */
    FRAME_MESSAGE_NACK,        /* 命令失败应答帧（NACK） */
} frame_message_type_t;

/*
 * 判断一帧消息的类型。
 *
 * 通过匹配 payload 中的类型字段（DATA/CMD/ACK/NACK）来判定，
 * 无法识别时返回 FRAME_MESSAGE_UNKNOWN。
 *
 * @param frame 已经通过CRC校验的帧
 *
 * @return 对应的帧类型枚举值
 */
frame_message_type_t frame_get_message_type(
    const parsed_frame_t *frame
);

#endif