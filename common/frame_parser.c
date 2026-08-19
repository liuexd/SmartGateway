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
 * 解析一个通用 KV 帧的 payload（内部共用）。
 *
 * DATA / CMD / ACK 的固定头部都是 4 个字段：
 *   1,<id>,<sequence>,<TYPE>
 *
 * 其后跟随任意数量的 KEY=VALUE（最多 FRAME_DATA_MAX_FIELDS 条）。
 * 解析器只负责“搬运”，不解释任何 key 的业务含义。
 *
 * @param frame      已经通过CRC校验的帧
 * @param type       "DATA" / "CMD" / "ACK"，用于校验类型字段
 * @param id_out     输出：节点编号/发送方（要求容量 >= id_capacity）
 * @param id_capacity id_out 的容量
 * @param sequence_out 输出：帧序号
 * @param fields     输出：键值对数组
 * @param field_count_out 输出：实际字段数量
 *
 * @return 成功返回0，失败返回-1
 */
static int parse_kv_payload(
    const parsed_frame_t *frame,
    const char *type,
    char *id_out,
    size_t id_capacity,
    uint32_t *sequence_out,
    frame_kv_t *fields,
    size_t *field_count_out)
{
    char copy[FRAME_MAX_LEN];
    char *tokens[4 + FRAME_DATA_MAX_FIELDS];
    char *save_ptr = NULL;
    char *token;
    size_t count = 0;
    size_t i;

    if (frame == NULL || type == NULL ||
        id_out == NULL || id_capacity == 0U ||
        sequence_out == NULL || fields == NULL ||
        field_count_out == NULL)
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
    memcpy(copy, frame->payload, frame->payload_length + 1U);

    token = strtok_r(copy, ",", &save_ptr);

    while (token != NULL &&
           count < 4U + FRAME_DATA_MAX_FIELDS)
    {
        tokens[count] = token;
        count++;

        token = strtok_r(NULL, ",", &save_ptr);
    }

    /*
     * 至少要有固定头部 4 个字段；
     * 数据字段超过容量上限时拒绝。
     */
    if (count < 4U || token != NULL)
    {
        return -1;
    }

    /*
     * 检查固定字段：版本号、类型。
     */
    if (strcmp(tokens[0], "1") != 0)
    {
        return -1;
    }

    if (strcmp(tokens[3], type) != 0)
    {
        return -1;
    }

    /*
     * 节点编号/发送方不能为空，也不能超过输出空间。
     */
    if (tokens[1][0] == '\0' ||
        strlen(tokens[1]) >= id_capacity)
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
    if (parse_uint32_decimal(tokens[2], sequence_out) != 0)
    {
        return -1;
    }

    strcpy(id_out, tokens[1]);

    /*
     * 解析数据区：tokens[4..count-1] 每条必须是 KEY=VALUE。
     */
    for (i = 4; i < count; i++)
    {
        const char *kv = tokens[i];
        const char *eq;
        size_t key_len;
        size_t value_len;

        eq = strchr(kv, '=');

        /*
         * 必须形如 KEY=VALUE，且 key/value 都不能为空。
         */
        if (eq == NULL || eq == kv || eq[1] == '\0')
        {
            return -1;
        }

        key_len = (size_t)(eq - kv);
        value_len = strlen(eq + 1);

        if (key_len >= FRAME_KV_KEY_MAX ||
            value_len >= FRAME_KV_VALUE_MAX)
        {
            return -1;
        }

        memcpy(fields[i - 4U].key, kv, key_len);
        fields[i - 4U].key[key_len] = '\0';

        strcpy(fields[i - 4U].value, eq + 1);
    }

    *field_count_out = count - 4U;

    return 0;
}

int frame_decode_data(
    const parsed_frame_t *frame,
    frame_data_t *out)
{
    uint32_t sequence;
    size_t field_count;

    if (frame == NULL || out == NULL)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (parse_kv_payload(
            frame,
            "DATA",
            out->node_id,
            sizeof(out->node_id),
            &sequence,
            out->fields,
            &field_count) != 0)
    {
        return -1;
    }

    out->sequence = sequence;
    out->field_count = field_count;

    return 0;
}

const char *frame_fields_find(
    const frame_kv_t *fields,
    size_t field_count,
    const char *key)
{
    size_t i;

    if (fields == NULL ||
        key == NULL ||
        key[0] == '\0')
    {
        return NULL;
    }

    for (i = 0; i < field_count; i++)
    {
        if (strcmp(fields[i].key, key) == 0)
        {
            return fields[i].value;
        }
    }

    return NULL;
}

const char *frame_data_find_field(
    const frame_data_t *data,
    const char *key)
{
    if (data == NULL)
    {
        return NULL;
    }

    return frame_fields_find(
        data->fields,
        data->field_count,
        key);
}

int frame_decode_command(
    const parsed_frame_t *frame,
    frame_command_t *out)
{
    uint32_t sequence;
    size_t field_count;

    if (frame == NULL || out == NULL)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (parse_kv_payload(
            frame,
            "CMD",
            out->sender,
            sizeof(out->sender),
            &sequence,
            out->fields,
            &field_count) != 0)
    {
        return -1;
    }

    out->sequence = sequence;
    out->field_count = field_count;

    return 0;
}

int frame_decode_ack(
    const parsed_frame_t *frame,
    frame_ack_t *out)
{
    uint32_t sequence;
    size_t field_count;

    if (frame == NULL || out == NULL)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    if (parse_kv_payload(
            frame,
            "ACK",
            out->node_id,
            sizeof(out->node_id),
            &sequence,
            out->fields,
            &field_count) != 0)
    {
        return -1;
    }

    out->sequence = sequence;
    out->field_count = field_count;

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
     * 1,NODE01,000001,DATA,T=253,H=601,P=1013
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