#ifndef COMMAND_MANAGER_H
#define COMMAND_MANAGER_H

#include "frame_parser.h"

#include <stddef.h>
#include <stdint.h>
#include <time.h>

/*
 * =====================================================================
 * 命令管理器（command_manager）
 *
 * 职责：
 *   1. 统一分配命令序号（sequence）；
 *   2. 跟踪已下发命令的ACK/NACK应答状态；
 *   3. 超时未应答的命令可重发（重试耗尽后标记超时失败）。
 *
 * 设计：
 *   - 只管理状态，不碰网络；组帧、TCP发送由调用方完成；
 *   - 用定长数组 + 线性查找（命令量小，O(n)足够）；
 *   - 序号与 ACK/NACK 应答通过 sequence 配对。
 * =====================================================================
 */

/*
 * 命令状态。
 *
 * COMMAND_STATE_FREE   ：槽位空闲
 * COMMAND_STATE_WAITING：已下发，等待ACK/NACK
 * COMMAND_STATE_ACKED  ：设备执行成功
 * COMMAND_STATE_NACKED ：设备执行失败
 * COMMAND_STATE_TIMEOUT：重试耗尽仍未应答
 */
typedef enum
{
    COMMAND_STATE_FREE = 0,
    COMMAND_STATE_WAITING,
    COMMAND_STATE_ACKED,
    COMMAND_STATE_NACKED,
    COMMAND_STATE_TIMEOUT
} command_state_t;

/*
 * 命令管理器句柄（不透明类型）。
 */
typedef struct command_manager command_manager_t;

/*
 * 创建命令管理器。
 *
 * @return 成功返回句柄，失败返回NULL
 */
command_manager_t *command_manager_create(void);

/*
 * 销毁命令管理器，释放全部资源。
 */
void command_manager_destroy(command_manager_t *manager);

/*
 * 生成一帧CMD命令帧。
 *
 * 直接复用 frame_build_command() 组帧。
 *
 * @param out       输出缓冲区
 * @param out_size  输出缓冲区大小
 * @param sender    发送方标识（网关编号）
 * @param sequence  命令序号（由 command_manager_send() 分配）
 * @param led_value LED控制值
 *
 * @return 成功返回帧长度，失败返回负数
 */
int command_manager_build_cmd(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    int led_value
);

/*
 * 下发一条命令：分配序号并记录状态。
 *
 * 调用方拿到 sequence 后自行组帧/发送。
 *
 * @param manager      命令管理器
 * @param sender       发送方标识（网关编号），例如 "GATEWAY"
 * @param led_value    LED控制值
 * @param sequence_out 输出：分配的命令序号
 *
 * @return 0成功；-1参数错误或表满
 */
int command_manager_send(
    command_manager_t *manager,
    const char *sender,
    int led_value,
    uint32_t *sequence_out
);

/*
 * 处理节点返回的ACK应答。
 *
 * 按 ack->sequence 找到对应命令，标记为已确认。
 *
 * @return 0找到并处理；-1未找到对应命令或参数错误
 */
int command_manager_on_ack(
    command_manager_t *manager,
    const frame_ack_t *ack
);

/*
 * 处理节点返回的NACK应答。
 *
 * 按 nack->sequence 找到对应命令，记录失败原因。
 *
 * @return 0找到并处理；-1未找到对应命令或参数错误
 */
int command_manager_on_nack(
    command_manager_t *manager,
    const frame_nack_t *nack
);

/*
 * 查询一条命令的当前状态。
 *
 * @param manager    命令管理器
 * @param sequence   命令序号
 * @param state_out  输出：命令状态
 *
 * @return 0找到；-1未找到或参数错误
 */
int command_manager_query(
    const command_manager_t *manager,
    uint32_t sequence,
    command_state_t *state_out
);

/*
 * 检查超时未应答的命令。
 *
 * 对处于 WAITING 且超过 timeout_sec 的命令：
 *   - 重试次数未达 max_retries：重发（状态保持WAITING，retries+1），
 *     序号和led值分别写入 retry_sequences / retry_leds 数组；
 *   - 重试次数已耗尽：标记为 TIMEOUT，序号写入 timeout_sequences 数组。
 *
 * @param manager           命令管理器
 * @param now               当前时间（time(NULL)）
 * @param timeout_sec       超时阈值（秒）
 * @param max_retries       最大重试次数
 * @param retry_sequences   输出：需要重发的序号列表（可NULL）
 * @param retry_leds        输出：与retry_sequences对应的led值（可NULL）
 * @param retry_capacity    retry_sequences/retry_leds 容量
 * @param retry_count       输出：重发数量（可NULL）
 * @param timeout_sequences 输出：重试耗尽的序号列表（可NULL）
 * @param timeout_capacity  timeout_sequences 容量
 * @param timeout_count     输出：超时失败数量（可NULL）
 *
 * @return 0正常执行；-1参数错误
 */
int command_manager_check_timeouts(
    command_manager_t *manager,
    time_t now,
    int timeout_sec,
    int max_retries,
    uint32_t *retry_sequences,
    int *retry_leds,
    size_t retry_capacity,
    size_t *retry_count,
    uint32_t *timeout_sequences,
    size_t timeout_capacity,
    size_t *timeout_count
);

#endif
