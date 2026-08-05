#include "line_parser.h"

#include <string.h>

/*
 * 从累计缓冲区中寻找以'\n'结束的完整消息。
 *
 * TCP没有消息边界：
 * 一次recv可能只有半条JSON，也可能包含多条JSON。
 *
 * 实现要点：
 * - 用memchr查找'\n'，找不到说明当前只有半条消息，直接返回；
 * - 去掉行尾的'\r'（兼容\r\n换行）；
 * - 调用回调后，把已消费的字节前移（memmove），
 *   剩余的半条消息保留在缓冲区开头。
 */
size_t line_parser_feed(
    char *buffer,
    size_t *buffer_length,
    size_t buffer_capacity,
    line_parser_callback_t callback,
    void *user_data
)
{
    size_t lines = 0;

    if (buffer == NULL ||
        buffer_length == NULL ||
        buffer_capacity == 0U)
    {
        return 0;
    }

    while (*buffer_length > 0U)
    {
        char *newline;
        size_t line_length;
        size_t consumed;

        /*
         * 在当前有效数据中查找'\n'。
         */
        newline = memchr(
            buffer,
            '\n',
            *buffer_length
        );

        if (newline == NULL)
        {
            /*
             * 剩余数据中没有'\n'，
             * 说明只是半条消息，保留等待下一次数据。
             */
            break;
        }

        /*
         * 消息长度为'\n'之前的字节数。
         *
         * 如果消息以'\r\n'结尾，则去掉'\r'。
         */
        line_length = (size_t)(newline - buffer);

        if (line_length > 0U &&
            buffer[line_length - 1U] == '\r')
        {
            line_length--;
        }

        if (callback != NULL)
        {
            callback(buffer, line_length, user_data);
        }

        lines++;

        /*
         * 已消费的字节数 = '\n'之前的数据 + '\n'本身。
         */
        consumed = (size_t)(newline - buffer) + 1U;

        /*
         * 把剩余数据前移到缓冲区开头，
         * 供下一次调用继续处理。
         */
        if (consumed < *buffer_length)
        {
            memmove(
                buffer,
                newline + 1,
                *buffer_length - consumed
            );
        }

        *buffer_length -= consumed;
    }

    return lines;
}
