#ifndef MESSAGE_JSON_H
#define MESSAGE_JSON_H

#include "frame_parser.h"

#include <stddef.h>

/*
 * =====================================================================
 * 消息JSON序列化
 *
 * 把协议解析结果（frame_*_t 结构体）转换为JSON字符串，
 * 供网关通过TCP上报给服务器。
 * =====================================================================
 */

/*
 * 将DATA帧解析结果转换为JSON字符串。
 *
 * 生成格式（数据区为通用键值对对象）：
 * {"node":"NODE01","seq":1,"type":"DATA","fields":{"T":"253","H":"601"}}
 *
 * 值统一按字符串输出，保持与串口帧一致的原始精度。
 *
 * @param out     输出缓冲区
 * @param out_size 输出缓冲区大小
 * @param data    已解析的DATA帧数据
 *
 * @return 成功返回写入的字节数（不含'\0'），失败返回负数
 */
int message_json_build_data(
    char *out,
    size_t out_size,
    const frame_data_t *data
);

/*
 * 将ACK帧解析结果转换为JSON字符串。
 *
 * 生成格式：
 * {"node":"NODE01","seq":1,"type":"ACK","led":1}
 *
 * @return 成功返回写入的字节数（不含'\0'），失败返回负数
 */
int message_json_build_ack(
    char *out,
    size_t out_size,
    const frame_ack_t *ack
);

/*
 * 将NACK帧解析结果转换为JSON字符串。
 *
 * 生成格式：
 * {"node":"NODE01","seq":1,"type":"NACK","error":"bad_cmd"}
 *
 * @return 成功返回写入的字节数（不含'\0'），失败返回负数
 */
int message_json_build_nack(
    char *out,
    size_t out_size,
    const frame_nack_t *nack
);
/*
 * 将CMD帧解析结果转换为JSON字符串。
 *
 * 生成格式（命令参数为通用键值对对象）：
 * {"node":"GATEWAY","seq":1,"type":"CMD","fields":{"LED":"1","MOTOR":"50"}}
 *
 * @return 成功返回写入的字节数（不含'\0'），失败返回负数
 */
int message_json_build_command(
    char *out,
    size_t out_size,
    const frame_command_t *command
);
/*
 * 将JSON字符串转换为CMD帧解析结果。
 *
 * 支持两种输入格式：
 *
 * 1. 新格式（推荐，命令参数为通用键值对对象）：
 *    {"node":"GATEWAY","seq":1,"type":"CMD","fields":{"LED":"1","MOTOR":"50"}}
 *
 * 2. 旧格式（兼容，单个 led 参数，自动转为 LED 键值对）：
 *    {"node":"GATEWAY","seq":1,"type":"CMD","led":1}
 *
 * 必需字段为 node/seq/type；fields 对象可选。
 * 无关字段会被忽略，缺失必需字段或type不是"CMD"时解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param command     解析结果输出（成功时填充sender/sequence/fields，
 *                    失败时清零）
 *
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回负数
 */
int message_json_decode_command(
    const char * line,
    size_t line_length,
    frame_command_t *command
);

/*
 * 将JSON字符串转换为ACK帧解析结果。
 *
 * 支持两种输入格式：
 *
 * 1. 新格式（推荐，回显为通用键值对对象）：
 *    {"node":"NODE01","seq":1,"type":"ACK","fields":{"LED":"1"}}
 *
 * 2. 旧格式（兼容，单个 led 参数，自动转为 LED 键值对）：
 *    {"node":"NODE01","seq":1,"type":"ACK","led":1}
 *
 * 必需字段为 node/seq/type；fields 对象可选。
 * 无关字段会被忽略，缺失必需字段或type不是"ACK"时解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param ack         解析结果输出（成功时填充node_id/sequence/fields，
 *                    失败时清零）
 *
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回负数
 */
int message_json_decode_ack(
    const char * line,
    size_t line_length,
    frame_ack_t *ack
);

/*
 * 将JSON字符串转换为NACK帧解析结果。
 *
 * 输入格式与 message_json_build_nack() 的生成格式一致：
 * {"node":"NODE01","seq":1,"type":"NACK","error":"bad_cmd"}
 *
 * 无关字段会被忽略，缺失必需字段或type不是"NACK"时解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param nack        解析结果输出（成功时填充node_id/sequence/error，
 *                    失败时清零）
 *
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回负数
 */
int message_json_decode_nack(
    const char * line,
    size_t line_length,
    frame_nack_t *nack
);

/*
 * 将JSON字符串转换为DATA帧解析结果。
 *
 * 支持两种输入格式：
 *
 * 1. 新格式（推荐，数据区为通用键值对对象）：
 *    {"node":"NODE01","seq":1,"type":"DATA","fields":{"T":"253","H":"601"}}
 *
 * 2. 旧格式（兼容，temperature/humidity 为数字，解析后放大10倍
 *    转成 T/H 两条键值对）：
 *    {"node":"NODE01","seq":1,"type":"DATA","temperature":25.3,"humidity":60.1}
 *
 * 必需字段为 node/seq/type；fields 对象可选，其中每个键值对
 * 都会原样存入 data->fields。
 * 无关字段会被忽略，缺失必需字段或type不是"DATA"时解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param data        解析结果输出（成功时填充node_id/sequence/fields，
 *                    失败时清零）
 *
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回负数
 */
int message_json_decode_data(
    const char * line,
    size_t line_length,
    frame_data_t *data
);

#endif
