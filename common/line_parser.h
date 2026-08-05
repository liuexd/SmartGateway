#ifndef LINE_PARSER_H
#define LINE_PARSER_H

#include <stddef.h>

/*
 * 行解析回调：每解析出一条以'\n'结尾的完整消息时调用一次。
 *
 * @param line      完整消息内容（不含'\n'，已去除结尾的'\r'）
 * @param length    消息长度（不含'\n'和'\r'）
 * @param user_data 调用 line_parser_feed() 时透传的用户数据
 */
typedef void (*line_parser_callback_t)(
    const char *line,
    size_t length,
    void *user_data
);

/*
 * 从累计缓冲区中提取以'\n'结尾的完整消息。
 *
 * TCP/串口没有消息边界：
 * 一次recv可能只有半条消息，也可能包含多条消息。
 *
 * 本函数负责：
 *   1. 在累计缓冲区中查找'\n'；
 *   2. 对每条完整消息，去掉结尾的'\r'后调用callback；
 *   3. 把已消费的字节从缓冲区中移除，剩余半条消息保留。
 *
 * @param buffer        累计缓冲区（必须能存放 buffer_capacity 字节）
 * @param buffer_length 输入：当前缓冲区中有效字节数；输出：剩余未消费字节数
 * @param buffer_capacity 缓冲区总容量
 * @param callback      每解析出一条完整消息时调用
 * @param user_data     透传给回调的用户数据
 *
 * @return 本次调用解析出的完整消息条数
 */
size_t line_parser_feed(
    char *buffer,
    size_t *buffer_length,
    size_t buffer_capacity,
    line_parser_callback_t callback,
    void *user_data
);

#endif
