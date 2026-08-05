#ifndef FRAME_H
#define FRAME_H

#include <stddef.h>
#include <stdint.h>

/* 协议规定的最大帧长度，包含结尾的 '\0' */
#define FRAME_MAX_LEN 256

/* frame_build_data() 的错误返回值 */
typedef enum
{
    FRAME_ERR_PARAM = -1,
    FRAME_ERR_TOO_LONG = -2,
    FRAME_ERR_NODE_ID = -3
} frame_error_t;

/**
 * @brief 生成一帧温湿度数据
 *
 * 生成格式：
 * @1,NODE01,000001,DATA,T=253,H=601*CCCC\r\n
 *
 * CRC 计算范围：
 * 1,NODE01,000001,DATA,T=253,H=601
 *
 * @param out             输出缓冲区
 * @param out_size        输出缓冲区大小
 * @param node_id         节点编号，例如 "NODE01"
 * @param sequence        帧序号，范围 0~999999
 * @param temperature_x10 温度放大 10 倍后的整数，例如 253 表示 25.3℃
 * @param humidity_x10    湿度放大 10 倍后的整数，例如 601 表示 60.1%
 *
 * @return 成功时返回帧的实际长度，不包含字符串结尾 '\0'
 *         失败时返回负数错误码
 */
int frame_build_data(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int temperature_x10,
    int humidity_x10
);

/*
 * =====================================================================
 * 命令/应答（ACK/NACK）协议
 * =====================================================================
 *
 * 网关 -> 节点：CMD 帧（frame_command_t），用于下发控制指令
 * 节点 -> 网关：ACK 帧（frame_ack_t），表示命令执行成功
 * 节点 -> 网关：NACK 帧（frame_nack_t），表示命令执行失败
 *
 * 三者通过相同的 sequence 序号相互配对，
 * 网关收到应答后即可确定对应的是哪一条命令。
 * =====================================================================
 */

/*
 * 网关下发的控制命令数据。
 *
 * sender：命令发送方标识（网关编号），例如 "GW01"
 * sequence：命令序号，用于和节点的 ACK/NACK 应答配对
 * led_value：命令参数，例如控制 LED 的亮灭（0=灭，1=亮）
 */
typedef struct 
{
    char sender[32];
    uint32_t sequence;
    int led_value;
} frame_command_t;

/*
 * 节点对命令的成功应答（Acknowledgment）。
 *
 * node_id：应答的节点编号，例如 "NODE01"
 * sequence：所应答命令的序号，与 frame_command_t 中的序号一致
 * led_value：执行后 LED 的实际状态，用于回显确认
 */
typedef struct 
{
    char node_id[32];
    uint32_t sequence;
    int led_value;
} frame_ack_t;

/*
 * 节点对命令的失败应答（Negative Acknowledgment）。
 *
 * node_id：应答的节点编号，例如 "NODE01"
 * sequence：所应答命令的序号，与 frame_command_t 中的序号一致
 * error：失败原因描述字符串，例如 "bad_crc"、"unsupported_cmd"
 */
typedef struct 
{
    char node_id[32];
    uint32_t sequence;
    char error[32];
} frame_nack_t;

/*
 * 生成一帧 CMD（命令）帧。
 *
 * 帧格式示例：
 * @1,GW01,000001,CMD,LED=1*CCCC\r\n
 *
 * @param out      输出缓冲区
 * @param out_size 输出缓冲区大小
 * @param sequence 命令序号，范围 0~999999
 * @param led_value LED 控制值（任意整数；协议层不校验合法性，
 *                  具体语义/合法范围由设备侧决定，0=灭，1=亮）
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_command(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    int led_value
);

/*
 * 生成一帧 ACK（成功应答）帧。
 *
 * 帧格式示例：
 * @NODE01,000001,ACK,LED=1*CCCC\r\n
 *
 * @param out       输出缓冲区
 * @param out_size  输出缓冲区大小
 * @param node_id   节点编号，例如 "NODE01"
 * @param sequence  所应答命令的序号，必须与命令帧一致
 * @param led_value 执行后 LED 的实际状态（协议层不校验合法性）
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_ack(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int led_value
);

/*
 * 生成一帧 NACK（失败应答）帧。
 *
 * 帧格式示例：
 * @NODE01,000001,NACK,ERR=unsupported_cmd*CCCC\r\n
 *
 * @param out      输出缓冲区
 * @param out_size 输出缓冲区大小
 * @param node_id  节点编号，例如 "NODE01"
 * @param sequence 所应答命令的序号，必须与命令帧一致
 * @param error    失败原因描述字符串
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
int frame_build_nack(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    const char *error
);


#endif