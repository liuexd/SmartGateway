/*
 * 启用strtok_r等POSIX接口声明。
 * 必须放在所有头文件之前。
 */
#define _POSIX_C_SOURCE 200809L

#include "frame_parser.h"
#include "crc16.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

/*
 * 将单个十六进制字符转换成数值。
 *
 * '0' -> 0
 * '9' -> 9
 * 'A' -> 10
 * 'F' -> 15
 */
static int hex_value(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        return ch - '0';
    }

    ch = (char)toupper((unsigned char)ch);

    if (ch >= 'A' && ch <= 'F')
    {
        return ch - 'A' + 10;
    }

    return -1;
}

/*
 * 将4位十六进制CRC文本转换为uint16_t。
 *
 * 例如：
 * "3F2E" -> 0x3F2E
 */
static int parse_crc4(const char *text, uint16_t *crc)
{
    uint16_t value = 0;
    size_t i;

    if (text == NULL || crc == NULL)
    {
        return -1;
    }

    for (i = 0; i < 4; i++)
    {
        int digit = hex_value(text[i]);

        if (digit < 0)
        {
            return -1;
        }

        value = (uint16_t)((value << 4) | (uint16_t)digit);
    }

    *crc = value;

    return 0;
}

/*
 * 检查一帧的基本格式和CRC。
 *
 * 返回值：
 *  0：格式正确且CRC正确
 *  1：格式正确但CRC错误
 * -1：帧格式错误
 */
static int validate_frame(
    const char *buffer,
    size_t length,
    parsed_frame_t *out)
{
    const char *star;
    size_t star_index;
    size_t payload_length;

    uint16_t received_crc;
    uint16_t calculated_crc;

    if (buffer == NULL || out == NULL)
    {
        return -1;
    }

    /*
     * 最短也需要：
     *
     * @1*A1B2\r\n
     */
    if (length < 8 || length >= FRAME_MAX_LEN)
    {
        return -1;
    }

    /*
     * 必须以@开始，以\r\n结束。
     */
    if (buffer[0] != '@' ||
        buffer[length - 2] != '\r' ||
        buffer[length - 1] != '\n')
    {
        return -1;
    }

    /*
     * 寻找CRC分隔符*。
     *
     * 使用strrchr寻找最后一个*。
     */
    star = strrchr(buffer, '*');

    if (star == NULL)
    {
        return -1;
    }

    star_index = (size_t)(star - buffer);

    /*
     * 帧尾必须严格为：
     *
     * *CCCC\r\n
     *
     * 从*开始一共7个字节：
     * 1个* + 4个CRC字符 + \r + \n
     */
    if (star_index + 7U != length)
    {
        return -1;
    }

    /*
     * @和*之间必须至少有一个有效字符。
     */
    if (star_index <= 1U)
    {
        return -1;
    }

    /*
     * 解析收到的4位CRC。
     */
    if (parse_crc4(star + 1, &received_crc) != 0)
    {
        return -1;
    }

    /*
     * payload从@后开始，到*前结束。
     */
    payload_length = star_index - 1U;

    /*
     * CRC计算范围：
     *
     * 不包括@
     * 不包括*
     * 不包括CRC文本
     * 不包括\r\n
     */
    calculated_crc = crc16_ccitt_false(
        (const uint8_t *)(buffer + 1),
        payload_length);

    if (calculated_crc != received_crc)
    {
        return 1;
    }

    /*
     * CRC正确，保存完整帧。
     */
    memcpy(out->raw, buffer, length);
    out->raw[length] = '\0';
    out->raw_length = length;

    /*
     * 保存参与CRC计算的数据区。
     */
    memcpy(
        out->payload,
        buffer + 1,
        payload_length);

    out->payload[payload_length] = '\0';
    out->payload_length = payload_length;

    out->received_crc = received_crc;
    out->calculated_crc = calculated_crc;

    return 0;
}

void frame_parser_init(frame_parser_t *parser)
{
    if (parser == NULL)
    {
        return;
    }

    /*
     * 清零接收状态和统计数据。
     */
    memset(parser, 0, sizeof(*parser));
}

void frame_parser_reset(frame_parser_t *parser)
{
    if (parser == NULL)
    {
        return;
    }

    /*
     * 只清除当前正在接收的帧。
     * 不清除stats统计数据。
     */
    parser->length = 0;
    parser->collecting = 0;
    parser->buffer[0] = '\0';
}

size_t frame_parser_feed(
    frame_parser_t *parser,
    const uint8_t *data,
    size_t data_length,
    frame_parser_callback_t callback,
    void *user_data)
{
    size_t i;
    size_t valid_count = 0;

    if (parser == NULL)
    {
        return 0;
    }

    if (data == NULL && data_length > 0U)
    {
        return 0;
    }

    /*
     * 无论传入1字节、10字节还是多帧数据，
     * 都逐字节交给状态机处理。
     */
    for (i = 0; i < data_length; i++)
    {
        char ch = (char)data[i];

        /*
         * 当前没有接收帧：
         * 忽略所有字符，直到找到@。
         */
        if (!parser->collecting)
        {
            if (ch == '@')
            {
                parser->collecting = 1;
                parser->length = 1;

                parser->buffer[0] = '@';
                parser->buffer[1] = '\0';
            }
            else
            {
                parser->stats.discarded_bytes++;
            }

            continue;
        }

        /*
         * 正在收帧时再次收到@。
         *
         * 说明上一帧可能损坏或只收到半帧，
         * 直接将这个@作为新帧开始。
         *
         * 例如：
         *
         * @1,NODE01,000001@1,NODE01,000002,...
         */
        if (ch == '@')
        {
            parser->stats.format_errors++;

            parser->length = 1;
            parser->buffer[0] = '@';
            parser->buffer[1] = '\0';

            continue;
        }

        /*
         * 缓冲区只剩字符串结尾'\0'的位置时，
         * 说明当前帧超过长度限制。
         */
        if (parser->length >= FRAME_MAX_LEN - 1U)
        {
            parser->stats.overflow_errors++;

            /*
             * 丢弃整个超长帧，重新寻找帧头。
             */
            frame_parser_reset(parser);

            continue;
        }

        /*
         * 把当前字符放入帧缓冲区。
         */
        parser->buffer[parser->length] = ch;
        parser->length++;

        /*
         * 始终保证它是合法C字符串，
         * 方便后面使用strrchr等函数。
         */
        parser->buffer[parser->length] = '\0';

        /*
         * 收到\n，表示一帧可能结束。
         */
        if (ch == '\n')
        {
            parsed_frame_t frame;
            int result;

            memset(&frame, 0, sizeof(frame));

            result = validate_frame(
                parser->buffer,
                parser->length,
                &frame);

            if (result == 0)
            {
                /*
                 * 帧格式和CRC都正确。
                 */
                parser->stats.valid_frames++;
                valid_count++;

                if (callback != NULL)
                {
                    callback(&frame, user_data);
                }
            }
            else if (result == 1)
            {
                /*
                 * 格式基本正确，但CRC不一致。
                 */
                parser->stats.crc_errors++;
            }
            else
            {
                /*
                 * 帧尾、CRC字符数量等格式错误。
                 */
                parser->stats.format_errors++;
            }

            /*
             * 无论成功还是失败，
             * 当前帧都已经处理完成。
             */
            frame_parser_reset(parser);
        }
    }

    return valid_count;
}

/*
 * 将十进制字符串转换成uint32_t。
 */
static int parse_uint32_decimal(
    const char *text,
    uint32_t *value)
{
    char *end;
    unsigned long parsed;

    if (text == NULL ||
        value == NULL ||
        text[0] == '\0')
    {
        return -1;
    }

    errno = 0;
    end = NULL;

    parsed = strtoul(text, &end, 10);

    /*
     * 检查：
     * 1. 是否发生转换错误；
     * 2. 是否完全没有转换；
     * 3. 后面是否残留非法字符；
     * 4. 是否超过uint32_t范围。
     */
    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        parsed > UINT32_MAX)
    {
        return -1;
    }

    *value = (uint32_t)parsed;

    return 0;
}

/*
 * 将可能包含负号的十进制字符串转换成int。
 */
static int parse_int_decimal(
    const char *text,
    int *value)
{
    char *end;
    long parsed;

    if (text == NULL ||
        value == NULL ||
        text[0] == '\0')
    {
        return -1;
    }

    errno = 0;
    end = NULL;

    parsed = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        parsed < INT_MIN ||
        parsed > INT_MAX)
    {
        return -1;
    }

    *value = (int)parsed;

    return 0;
}

int frame_decode_data(
    const parsed_frame_t *frame,
    frame_data_t *out)
{
    char copy[FRAME_MAX_LEN];

    /*
     * DATA帧应该有6个字段：
     *
     * 1
     * NODE01
     * 000001
     * DATA
     * T=253
     * H=601
     */
    char *tokens[6];

    char *save_ptr = NULL;
    char *token;
    size_t count = 0;

    if (frame == NULL || out == NULL)
    {
        return -1;
    }

    if (frame->payload_length >= sizeof(copy))
    {
        return -1;
    }

    /*
     * strtok_r会修改字符串，
     * 因此不能直接修改frame->payload。
     */
    memcpy(
        copy,
        frame->payload,
        frame->payload_length + 1U);

    memset(out, 0, sizeof(*out));

    token = strtok_r(copy, ",", &save_ptr);

    while (token != NULL && count < 6U)
    {
        tokens[count] = token;
        count++;

        token = strtok_r(NULL, ",", &save_ptr);
    }

    /*
     * 必须正好6个字段。
     *
     * count不等于6表示字段过少。
     * token不为NULL表示字段过多。
     */
    if (count != 6U || token != NULL)
    {
        return -1;
    }

    /*
     * 检查固定字段。
     */
    if (strcmp(tokens[0], "1") != 0)
    {
        return -1;
    }

    if (strcmp(tokens[3], "DATA") != 0)
    {
        return -1;
    }

    if (strncmp(tokens[4], "T=", 2) != 0)
    {
        return -1;
    }

    if (strncmp(tokens[5], "H=", 2) != 0)
    {
        return -1;
    }

    /*
     * 节点编号不能为空，也不能超过结构体空间。
     */
    if (tokens[1][0] == '\0' ||
        strlen(tokens[1]) >= sizeof(out->node_id))
    {
        return -1;
    }

    /*
     * 第一版协议规定序号必须是6位。
     */
    if (strlen(tokens[2]) != 6U)
    {
        return -1;
    }

    /*
     * 将文本数字转换成整数。
     */
    if (parse_uint32_decimal(
            tokens[2],
            &out->sequence) != 0)
    {
        return -1;
    }

    if (parse_int_decimal(
            tokens[4] + 2,
            &out->temperature_x10) != 0)
    {
        return -1;
    }

    if (parse_int_decimal(
            tokens[5] + 2,
            &out->humidity_x10) != 0)
    {
        return -1;
    }

    strcpy(out->node_id, tokens[1]);

    return 0;
}

int frame_decode_command(
    const parsed_frame_t *frame,
    frame_command_t *out)
{
    char copy[FRAME_MAX_LEN];
    char *token;
    char *tokens[5];
    char *save_pstr = NULL;
    size_t count = 0;
    if (frame == NULL || out == NULL)
    {
        return -1;
    }
    if (frame->payload_length >= sizeof(copy))
    {
        return -1;
    }
    memcpy(copy, frame->payload, frame->payload_length + 1U);
    memset(out, 0, sizeof(*out));

    token = strtok_r(copy, ",", &save_pstr);
    while (token != NULL && count < 5U)
    {
        tokens[count++] = token;
        token = strtok_r(NULL, ",", &save_pstr);
    }
    if (count != 5U || token != NULL)
    {
        return -1;
    }
    /*
     * 检查固定字段。
     */
    if (strcmp(tokens[0], "1") != 0)
    {
        return -1;
    }

    if (strcmp(tokens[3], "CMD") != 0)
    {
        return -1;
    }

    if (strncmp(tokens[4], "LED=", 4) != 0)
    {
        return -1;
    }
    /*
     * sender编号不能为空，也不能超过结构体空间。
     */
    if (tokens[1][0] == '\0' ||
        strlen(tokens[1]) >= sizeof(out->sender))
    {
        return -1;
    }

    /*
     * 第一版协议规定序号必须是6位。
     */
    if (strlen(tokens[2]) != 6U)
    {
        return -1;
    }

    /*
     * 将文本数字转换成整数。
     */
    if (parse_uint32_decimal(
            tokens[2],
            &out->sequence) != 0)
    {
        return -1;
    }

    if (parse_int_decimal(
            tokens[4] + 4,
            &out->led_value) != 0)
    {
        return -1;
    }

    strcpy(out->sender, tokens[1]);

    return 0;
}

int frame_decode_ack(
    const parsed_frame_t *frame,
    frame_ack_t *out)
{
    char copy[FRAME_MAX_LEN];
    char *token;
    char *tokens[5];
    char *save_pstr = NULL;
    size_t count = 0;
    if (frame == NULL || out == NULL)
    {
        return -1;
    }
    if (frame->payload_length >= sizeof(copy))
    {
        return -1;
    }
    memcpy(copy, frame->payload, frame->payload_length + 1U);
    memset(out, 0, sizeof(*out));

    token = strtok_r(copy, ",", &save_pstr);
    while (token != NULL && count < 5U)
    {
        tokens[count++] = token;
        token = strtok_r(NULL, ",", &save_pstr);
    }
    if (count != 5U || token != NULL)
    {
        return -1;
    }
    /*
     * 检查固定字段。
     */
    if (strcmp(tokens[0], "1") != 0)
    {
        return -1;
    }

    if (strcmp(tokens[3], "ACK") != 0)
    {
        return -1;
    }

    if (strncmp(tokens[4], "LED=", 4) != 0)
    {
        return -1;
    }
    /*
     * 节点编号不能为空，也不能超过结构体空间。
     */
    if (tokens[1][0] == '\0' ||
        strlen(tokens[1]) >= sizeof(out->node_id))
    {
        return -1;
    }

    /*
     * 第一版协议规定序号必须是6位。
     */
    if (strlen(tokens[2]) != 6U)
    {
        return -1;
    }

    /*
     * 将文本数字转换成整数。
     */
    if (parse_uint32_decimal(
            tokens[2],
            &out->sequence) != 0)
    {
        return -1;
    }

    if (parse_int_decimal(
            tokens[4] + 4,
            &out->led_value) != 0)
    {
        return -1;
    }

    strcpy(out->node_id, tokens[1]);

    return 0;
}

int frame_decode_nack(
    const parsed_frame_t *frame,
    frame_nack_t *out)
{
    char copy[FRAME_MAX_LEN];
    char *token;
    char *tokens[5];
    char *save_pstr = NULL;
    size_t count = 0;
    if (frame == NULL || out == NULL)
    {
        return -1;
    }
    if (frame->payload_length >= sizeof(copy))
    {
        return -1;
    }
    memcpy(copy, frame->payload, frame->payload_length + 1U);
    memset(out, 0, sizeof(*out));

    token = strtok_r(copy, ",", &save_pstr);
    while (token != NULL && count < 5U)
    {
        tokens[count++] = token;
        token = strtok_r(NULL, ",", &save_pstr);
    }
    if (count != 5U || token != NULL)
    {
        return -1;
    }
    /*
     * 检查固定字段。
     */
    if (strcmp(tokens[0], "1") != 0)
    {
        return -1;
    }

    if (strcmp(tokens[3], "NACK") != 0)
    {
        return -1;
    }

    if (strncmp(tokens[4], "ERR=", 4) != 0)
    {
        return -1;
    }
    /*
     * 节点编号不能为空，也不能超过结构体空间。
     */
    if (tokens[1][0] == '\0' ||
        strlen(tokens[1]) >= sizeof(out->node_id))
    {
        return -1;
    }

    /*
     * 第一版协议规定序号必须是6位。
     */
    if (strlen(tokens[2]) != 6U)
    {
        return -1;
    }

    /*
     * 将文本数字转换成整数。
     */
    if (parse_uint32_decimal(
            tokens[2],
            &out->sequence) != 0)
    {
        return -1;
    }

    /*
     * 错误描述不能为空，也不能超过结构体空间。
     */
    if (tokens[4][4] == '\0' ||
        strlen(tokens[4] + 4) >= sizeof(out->error))
    {
        return -1;
    }

    strcpy(out->error, tokens[4] + 4);
    strcpy(out->node_id, tokens[1]);

    return 0;
}

frame_message_type_t frame_get_message_type(
    const parsed_frame_t *frame)
{
    char copy[FRAME_MAX_LEN];
    char *token;
    char *save_pstr = NULL;
    int count = 0;

    if (frame == NULL)
    {
        return FRAME_MESSAGE_UNKNOWN;
    }
    if (frame->payload_length >= sizeof(copy))
    {
        return FRAME_MESSAGE_UNKNOWN;
    }
    memcpy(copy, frame->payload, frame->payload_length + 1U);

    /*
     * 帧类型是第4个字段（tokens[3]）：
     *
     * 1,NODE01,000001,DATA,T=253,H=601
     *            ^
     *           类型字段
     */
    token = strtok_r(copy, ",", &save_pstr);
    while (token != NULL && count < 3)
    {
        count++;
        token = strtok_r(NULL, ",", &save_pstr);
    }

    if (token == NULL)
    {
        return FRAME_MESSAGE_UNKNOWN;
    }

    if (strcmp(token, "DATA") == 0)
    {
        return FRAME_MESSAGE_DATA;
    }
    if (strcmp(token, "CMD") == 0)
    {
        return FRAME_MESSAGE_CMD;
    }
    if (strcmp(token, "ACK") == 0)
    {
        return FRAME_MESSAGE_ACK;
    }
    if (strcmp(token, "NACK") == 0)
    {
        return FRAME_MESSAGE_NACK;
    }

    return FRAME_MESSAGE_UNKNOWN;
}