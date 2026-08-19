#include "message_json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * 将DATA帧解析结果转换为JSON字符串。
 */
int message_json_build_data(
    char *out,
    size_t out_size,
    const frame_data_t *data
)
{
    size_t pos;
    size_t i;

    if (out == NULL || out_size == 0U || data == NULL)
    {
        return -1;
    }

    out[0] = '\0';

    /*
     * 固定前缀：
     * {"node":"NODE01","seq":1,"type":"DATA","fields":{
     */
    pos = (size_t)snprintf(
        out,
        out_size,
        "{\"node\":\"%s\","
        "\"seq\":%u,"
        "\"type\":\"DATA\","
        "\"fields\":{",
        data->node_id,
        (unsigned int)data->sequence);

    if (pos >= out_size)
    {
        out[0] = '\0';
        return -1;
    }

    /*
     * 数据区：每个字段输出为 "KEY":"VALUE"。
     *
     * key/value 来自串口帧解析，已经保证不含
     * 逗号/引号/反斜杠等JSON保留字符（协议层做过校验），
     * 因此这里不需要转义。
     */
    for (i = 0; i < data->field_count; i++)
    {
        int written;

        written = snprintf(
            out + pos,
            out_size - pos,
            "%s\"%s\":\"%s\"",
            (i > 0U) ? "," : "",
            data->fields[i].key,
            data->fields[i].value);

        if (written < 0)
        {
            out[0] = '\0';
            return -1;
        }

        if ((size_t)written >= out_size - pos)
        {
            out[0] = '\0';
            return -1;
        }

        pos += (size_t)written;
    }

    /*
     * 收尾：}} + 换行
     */
    {
        int written = snprintf(
            out + pos,
            out_size - pos,
            "}}\n");

        if (written < 0 ||
            (size_t)written >= out_size - pos)
        {
            out[0] = '\0';
            return -1;
        }

        pos += (size_t)written;
    }

    return (int)pos;
}

/*
 * 将ACK帧解析结果转换为JSON字符串。
 */
int message_json_build_ack(
    char *out,
    size_t out_size,
    const frame_ack_t *ack
)
{
    size_t pos;
    size_t i;

    if (out == NULL || out_size == 0U || ack == NULL)
    {
        return -1;
    }

    out[0] = '\0';

    /*
     * 固定前缀：
     * {"node":"NODE01","seq":1,"type":"ACK","fields":{
     */
    pos = (size_t)snprintf(
        out,
        out_size,
        "{\"node\":\"%s\","
        "\"seq\":%u,"
        "\"type\":\"ACK\","
        "\"fields\":{",
        ack->node_id,
        (unsigned int)ack->sequence);

    if (pos >= out_size)
    {
        out[0] = '\0';
        return -1;
    }

    /*
     * 回显状态：每个字段输出为 "KEY":"VALUE"。
     */
    for (i = 0; i < ack->field_count; i++)
    {
        int written;

        written = snprintf(
            out + pos,
            out_size - pos,
            "%s\"%s\":\"%s\"",
            (i > 0U) ? "," : "",
            ack->fields[i].key,
            ack->fields[i].value);

        if (written < 0)
        {
            out[0] = '\0';
            return -1;
        }

        if ((size_t)written >= out_size - pos)
        {
            out[0] = '\0';
            return -1;
        }

        pos += (size_t)written;
    }

    /*
     * 收尾：}} + 换行
     */
    {
        int written = snprintf(
            out + pos,
            out_size - pos,
            "}}\n");

        if (written < 0 ||
            (size_t)written >= out_size - pos)
        {
            out[0] = '\0';
            return -1;
        }

        pos += (size_t)written;
    }

    return (int)pos;
}

/*
 * 将NACK帧解析结果转换为JSON字符串。
 */
int message_json_build_nack(
    char *out,
    size_t out_size,
    const frame_nack_t *nack
)
{
    int json_length;

    if (out == NULL || out_size == 0U || nack == NULL)
    {
        return -1;
    }

    out[0] = '\0';

    json_length = snprintf(
        out,
        out_size,
        "{\"node\":\"%s\","
        "\"seq\":%u,"
        "\"type\":\"NACK\","
        "\"error\":\"%s\"}\n",
        nack->node_id,
        (unsigned int)nack->sequence,
        nack->error
    );

    if (json_length < 0)
    {
        out[0] = '\0';
        return -1;
    }

    if ((size_t)json_length >= out_size)
    {
        out[0] = '\0';
        return -1;
    }

    return json_length;
}
/*
 * 将CMD帧解析结果转换为JSON字符串。
 */
int message_json_build_command(
    char *out,
    size_t out_size,
    const frame_command_t *command
)
{
    size_t pos;
    size_t i;

    if (out == NULL || out_size == 0U || command == NULL)
    {
        return -1;
    }

    out[0] = '\0';

    /*
     * 固定前缀：
     * {"node":"GATEWAY","seq":1,"type":"CMD","fields":{
     */
    pos = (size_t)snprintf(
        out,
        out_size,
        "{\"node\":\"%s\","
        "\"seq\":%u,"
        "\"type\":\"CMD\","
        "\"fields\":{",
        command->sender,
        (unsigned int)command->sequence);

    if (pos >= out_size)
    {
        out[0] = '\0';
        return -1;
    }

    /*
     * 命令参数：每个字段输出为 "KEY":"VALUE"。
     *
     * key/value 来自协议层校验过的 KV，不含JSON保留字符，
     * 因此这里不需要转义。
     */
    for (i = 0; i < command->field_count; i++)
    {
        int written;

        written = snprintf(
            out + pos,
            out_size - pos,
            "%s\"%s\":\"%s\"",
            (i > 0U) ? "," : "",
            command->fields[i].key,
            command->fields[i].value);

        if (written < 0)
        {
            out[0] = '\0';
            return -1;
        }

        if ((size_t)written >= out_size - pos)
        {
            out[0] = '\0';
            return -1;
        }

        pos += (size_t)written;
    }

    /*
     * 收尾：}} + 换行
     */
    {
        int written = snprintf(
            out + pos,
            out_size - pos,
            "}}\n");

        if (written < 0 ||
            (size_t)written >= out_size - pos)
        {
            out[0] = '\0';
            return -1;
        }

        pos += (size_t)written;
    }

    return (int)pos;
}

/*
 * 跳过一段JSON空白字符（空格、制表符、\r、\n）。
 *
 * @param p   当前读取位置
 * @param end 缓冲区结束位置（不含）
 *
 * @return 跳过空白后的位置
 */
static const char *json_skip_whitespace(
    const char *p,
    const char *end)
{
    while (p < end &&
           (*p == ' ' || *p == '\t' ||
            *p == '\r' || *p == '\n'))
    {
        p++;
    }

    return p;
}

/*
 * 通用JSON对象解析结果（内部使用）。
 *
 * decode_command / decode_ack / decode_nack 共用同一解析器，
 * 解析出协议关心的全部字段，再由各函数校验必需字段和type。
 *
 * fields/found_fields 用于承载 CMD/ACK 的通用键值对参数：
 *   {"node":"GATEWAY","seq":1,"type":"CMD","fields":{"LED":"1","MOTOR":"50"}}
 */
typedef struct
{
    int found_node;
    char node_id[32];

    int found_seq;
    uint32_t sequence;

    int found_led;
    int led_value;

    int found_error;
    char error[32];

    int found_type;
    char type_value[8];

    int found_fields;
    frame_kv_t fields[FRAME_DATA_MAX_FIELDS];
    size_t field_count;
} json_object_t;

/*
 * 往键值对数组中追加一条字段（内部共用）。
 *
 * @param fields      目标数组
 * @param field_count 输入输出：数组当前长度
 * @param key         key 文本
 * @param key_len     key 长度
 * @param value       value 文本
 * @param value_len   value 长度
 *
 * @return 0成功，-1参数非法/超限/容量已满
 */
static int kv_array_add(
    frame_kv_t *fields,
    size_t *field_count,
    const char *key,
    size_t key_len,
    const char *value,
    size_t value_len)
{
    if (fields == NULL ||
        field_count == NULL ||
        key == NULL ||
        value == NULL)
    {
        return -1;
    }

    if (key_len == 0U ||
        key_len >= FRAME_KV_KEY_MAX ||
        value_len == 0U ||
        value_len >= FRAME_KV_VALUE_MAX ||
        *field_count >= FRAME_DATA_MAX_FIELDS)
    {
        return -1;
    }

    memcpy(fields[*field_count].key, key, key_len);
    fields[*field_count].key[key_len] = '\0';

    memcpy(fields[*field_count].value, value, value_len);
    fields[*field_count].value[value_len] = '\0';

    (*field_count)++;

    return 0;
}

/*
 * 解析一个JSON对象，提取协议关心的字段。
 *
 * 支持：
 *   {"node":"NODE01","seq":1,"type":"ACK","led":1}
 *   {"node":"NODE01","seq":1,"type":"NACK","error":"bad_cmd"}
 *   {"node":"GATEWAY","seq":1,"type":"CMD","fields":{"LED":"1","MOTOR":"50"}}
 *
 * "led":"1" 这种字符串形式的数字也兼容；
 * fields 对象中的键值对原样存入 out->fields；
 * 无关字段被忽略；结构非法（未闭合/键不是字符串/缺冒号等）返回-1。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param out         解析结果输出
 *
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回-1
 */
static int json_parse_object(
    const char *line,
    size_t line_length,
    json_object_t *out)
{
    const char *p;
    const char *end;
    int found_seq;
    int found_led;
    uint32_t sequence;
    int led_value;

    if (line == NULL ||
        line_length == 0U ||
        out == NULL)
    {
        return -1;
    }

    memset(out, 0, sizeof(*out));

    end = line + line_length;

    p = json_skip_whitespace(line, end);

    if (p >= end || *p != '{')
    {
        return -1;
    }

    p++;

    found_seq = 0;
    found_led = 0;
    sequence = 0;
    led_value = 0;

    for (;;)
    {
        const char *key_start;
        const char *key_end;
        int key_length;
        int is_node;
        int is_seq;
        int is_led;
        int is_error;
        int is_type;
        int is_fields;

        p = json_skip_whitespace(p, end);

        if (p >= end)
        {
            return -1;   /* JSON未闭合 */
        }

        if (*p == '}')
        {
            p++;
            break;       /* 对象结束 */
        }

        if (*p != '"')
        {
            return -1;   /* 键必须是字符串 */
        }

        /* 解析键名 */
        p++;
        key_start = p;

        while (p < end && *p != '"')
        {
            if (*p == '\\')
            {
                p++;
            }
            p++;
        }

        if (p >= end)
        {
            return -1;   /* 键字符串未闭合 */
        }

        key_end = p;
        p++;

        /* 期待 ':' */
        p = json_skip_whitespace(p, end);
        if (p >= end || *p != ':')
        {
            return -1;
        }
        p++;

        p = json_skip_whitespace(p, end);
        if (p >= end)
        {
            return -1;
        }

        key_length = (int)(key_end - key_start);
        is_node = (key_length == 4 &&
                   memcmp(key_start, "node", 4) == 0);
        is_seq = (key_length == 3 &&
                  memcmp(key_start, "seq", 3) == 0);
        is_led = (key_length == 3 &&
                  memcmp(key_start, "led", 3) == 0);
        is_error = (key_length == 5 &&
                    memcmp(key_start, "error", 5) == 0);
        is_type = (key_length == 4 &&
                   memcmp(key_start, "type", 4) == 0);
        is_fields = (key_length == 6 &&
                     memcmp(key_start, "fields", 6) == 0);

        if (is_fields)
        {
            /*
             * fields 值必须是对象：
             * "fields":{"LED":"1","MOTOR":"50"}
             *
             * 每个键值对原样存入 out->fields；
             * 值既可以是字符串，也可以是数字（原文存入）。
             */
            const char *fkey_start;
            const char *fkey_end;
            size_t fkey_len;

            if (*p != '{')
            {
                return -1;
            }
            p++;

            for (;;)
            {
                p = json_skip_whitespace(p, end);

                if (p >= end)
                {
                    return -1;   /* fields 未闭合 */
                }

                if (*p == '}')
                {
                    p++;
                    break;       /* fields 对象结束 */
                }

                if (*p != '"')
                {
                    return -1;   /* 字段键必须是字符串 */
                }

                /* 解析字段键名 */
                p++;
                fkey_start = p;

                while (p < end && *p != '"')
                {
                    if (*p == '\\')
                    {
                        p++;
                    }
                    p++;
                }

                if (p >= end)
                {
                    return -1;
                }

                fkey_end = p;
                p++;

                /* 期待 ':' */
                p = json_skip_whitespace(p, end);
                if (p >= end || *p != ':')
                {
                    return -1;
                }
                p++;

                p = json_skip_whitespace(p, end);
                if (p >= end)
                {
                    return -1;
                }

                fkey_len = (size_t)(fkey_end - fkey_start);

                if (*p == '"')
                {
                    /* 字符串值 */
                    const char *value_start;
                    const char *value_end;
                    size_t value_length;

                    p++;
                    value_start = p;

                    while (p < end && *p != '"')
                    {
                        if (*p == '\\')
                        {
                            p++;
                        }
                        p++;
                    }

                    if (p >= end)
                    {
                        return -1;   /* 值字符串未闭合 */
                    }

                    value_end = p;
                    p++;

                    value_length = (size_t)(value_end - value_start);

                    if (kv_array_add(
                            out->fields,
                            &out->field_count,
                            fkey_start,
                            fkey_len,
                            value_start,
                            value_length) != 0)
                    {
                        return -1;
                    }
                }
                else
                {
                    /* 数字值：原文存入 value */
                    const char *value_start = p;
                    size_t value_length;

                    if (*p == '-')
                    {
                        p++;
                    }

                    if (p >= end || *p < '0' || *p > '9')
                    {
                        return -1;
                    }

                    while (p < end && *p >= '0' && *p <= '9')
                    {
                        p++;
                    }

                    if (p < end && *p == '.')
                    {
                        p++;
                        if (p >= end || *p < '0' || *p > '9')
                        {
                            return -1;
                        }
                        while (p < end && *p >= '0' && *p <= '9')
                        {
                            p++;
                        }
                    }

                    if (p < end && (*p == 'e' || *p == 'E'))
                    {
                        p++;
                        if (p < end && (*p == '+' || *p == '-'))
                        {
                            p++;
                        }
                        if (p >= end || *p < '0' || *p > '9')
                        {
                            return -1;
                        }
                        while (p < end && *p >= '0' && *p <= '9')
                        {
                            p++;
                        }
                    }

                    value_length = (size_t)(p - value_start);

                    if (kv_array_add(
                            out->fields,
                            &out->field_count,
                            fkey_start,
                            fkey_len,
                            value_start,
                            value_length) != 0)
                    {
                        return -1;
                    }
                }

                /* 期待 ',' 或 '}' */
                p = json_skip_whitespace(p, end);

                if (p >= end)
                {
                    return -1;
                }

                if (*p == ',')
                {
                    p++;
                }
                else if (*p == '}')
                {
                    p++;
                    break;
                }
                else
                {
                    return -1;
                }
            }

            out->found_fields = 1;
        }
        else if (*p == '"')
        {
            /* 字符串值 */
            const char *value_start;
            const char *value_end;
            size_t value_length;

            p++;
            value_start = p;

            while (p < end && *p != '"')
            {
                if (*p == '\\')
                {
                    p++;
                }
                p++;
            }

            if (p >= end)
            {
                return -1;   /* 值字符串未闭合 */
            }

            value_end = p;
            p++;

            value_length = (size_t)(value_end - value_start);

            if (is_node)
            {
                if (value_length >= sizeof(out->node_id))
                {
                    return -1;   /* 节点标识过长 */
                }

                memcpy(out->node_id, value_start, value_length);
                out->node_id[value_length] = '\0';
                out->found_node = 1;
            }
            else if (is_type)
            {
                if (value_length >= sizeof(out->type_value))
                {
                    return -1;   /* 类型标识过长 */
                }

                memcpy(out->type_value, value_start, value_length);
                out->type_value[value_length] = '\0';
                out->found_type = 1;
            }
            else if (is_led)
            {
                /* 兼容 "led":"1" 这种字符串形式的数字 */
                char number[16];

                if (value_length == 0U ||
                    value_length >= sizeof(number))
                {
                    return -1;
                }

                memcpy(number, value_start, value_length);
                number[value_length] = '\0';
                led_value = atoi(number);
                found_led = 1;
            }
            else if (is_error)
            {
                if (value_length == 0U ||
                    value_length >= sizeof(out->error))
                {
                    return -1;   /* 错误描述过长 */
                }

                memcpy(out->error, value_start, value_length);
                out->error[value_length] = '\0';
                out->found_error = 1;
            }
            /* 其它未知字符串字段：值已跳过，忽略 */
        }
        else
        {
            /* 数字值 */
            unsigned long value;
            int negative;

            negative = 0;
            value = 0;

            if (*p == '-')
            {
                negative = 1;
                p++;
            }

            if (p >= end || *p < '0' || *p > '9')
            {
                return -1;   /* 不是合法数字 */
            }

            while (p < end && *p >= '0' && *p <= '9')
            {
                unsigned long digit;

                digit = (unsigned long)(*p - '0');

                if (value > (0xFFFFFFFFUL - digit) / 10U)
                {
                    return -1;   /* 超出uint32_t范围 */
                }

                value = value * 10U + digit;
                p++;
            }

            /* 小数部分：只消费，不影响结构体字段 */
            if (p < end && *p == '.')
            {
                p++;
                while (p < end && *p >= '0' && *p <= '9')
                {
                    p++;
                }
            }

            /* 指数部分：只消费 */
            if (p < end && (*p == 'e' || *p == 'E'))
            {
                p++;
                if (p < end && (*p == '+' || *p == '-'))
                {
                    p++;
                }
                while (p < end && *p >= '0' && *p <= '9')
                {
                    p++;
                }
            }

            if (is_seq)
            {
                if (negative)
                {
                    return -1;   /* 序号不允许为负 */
                }
                sequence = (uint32_t)value;
                found_seq = 1;
            }
            else if (is_led)
            {
                led_value = negative ? -(int)value : (int)value;
                found_led = 1;
            }
            /* 其它未知数字字段：值已跳过，忽略 */
        }

        /* 期待 ',' 或 '}' */
        p = json_skip_whitespace(p, end);

        if (p >= end)
        {
            return -1;
        }

        if (*p == ',')
        {
            p++;
        }
        else if (*p == '}')
        {
            p++;
            break;
        }
        else
        {
            return -1;
        }
    }

    out->found_seq = found_seq;
    out->sequence = sequence;
    out->found_led = found_led;
    out->led_value = led_value;

    return (int)(p - line);
}

/*
 * 将JSON字符串转换为CMD帧解析结果。
 *
 * 支持两种输入格式：
 *
 * 1. 新格式（推荐，命令参数为通用键值对对象）：
 *    {"node":"GATEWAY","seq":1,"type":"CMD","fields":{"LED":"1","MOTOR":"50"}}
 *
 * 2. 旧格式（兼容，单个 led 参数自动转为 LED 键值对）：
 *    {"node":"GATEWAY","seq":1,"type":"CMD","led":1}
 *
 * 必需字段为 node/seq/type；fields 对象可选，其中每个键值对
 * 都会原样存入 command->fields。
 * 无关字段会被忽略，缺失必需字段或type不是"CMD"时解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param command     解析结果输出（成功时填充sender/sequence/fields，
 *                    失败时清零）
 *
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回-1
 */
int message_json_decode_command(
    const char *line,
    size_t line_length,
    frame_command_t *command)
{
    json_object_t obj;
    int consumed;

    if (command == NULL)
    {
        return -1;
    }

    /* 失败时输出保持干净 */
    memset(command, 0, sizeof(*command));

    consumed = json_parse_object(line, line_length, &obj);

    if (consumed < 0)
    {
        return -1;
    }

    /* 必需字段必须齐全 */
    if (!obj.found_node || !obj.found_seq ||
        !obj.found_type)
    {
        return -1;
    }

    /* 类型必须是CMD */
    if (strcmp(obj.type_value, "CMD") != 0)
    {
        return -1;
    }

    if (strlen(obj.node_id) >= sizeof(command->sender))
    {
        return -1;
    }

    strcpy(command->sender, obj.node_id);
    command->sequence = obj.sequence;

    /*
     * 命令参数：
     * 优先使用 fields 对象；
     * 旧格式的单个 led 参数自动转为 LED 键值对。
     */
    if (obj.found_fields)
    {
        memcpy(command->fields, obj.fields, sizeof(obj.fields));
        command->field_count = obj.field_count;
    }
    else if (obj.found_led)
    {
        snprintf(command->fields[0].key, sizeof(command->fields[0].key), "LED");
        snprintf(command->fields[0].value, sizeof(command->fields[0].value), "%d", obj.led_value);
        command->field_count = 1;
    }
    else
    {
        command->field_count = 0;
    }

    return consumed;
}

/*
 * 将JSON字符串转换为ACK帧解析结果。
 *
 * 支持两种输入格式：
 *
 * 1. 新格式（推荐，回显为通用键值对对象）：
 *    {"node":"NODE01","seq":1,"type":"ACK","fields":{"LED":"1"}}
 *
 * 2. 旧格式（兼容，单个 led 参数自动转为 LED 键值对）：
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
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回-1
 */
int message_json_decode_ack(
    const char *line,
    size_t line_length,
    frame_ack_t *ack)
{
    json_object_t obj;
    int consumed;

    if (ack == NULL)
    {
        return -1;
    }

    /* 失败时输出保持干净 */
    memset(ack, 0, sizeof(*ack));

    consumed = json_parse_object(line, line_length, &obj);

    if (consumed < 0)
    {
        return -1;
    }

    /* 必需字段必须齐全 */
    if (!obj.found_node || !obj.found_seq ||
        !obj.found_type)
    {
        return -1;
    }

    /* 类型必须是ACK */
    if (strcmp(obj.type_value, "ACK") != 0)
    {
        return -1;
    }

    if (strlen(obj.node_id) >= sizeof(ack->node_id))
    {
        return -1;
    }

    strcpy(ack->node_id, obj.node_id);
    ack->sequence = obj.sequence;

    /*
     * 回显状态：
     * 优先使用 fields 对象；
     * 旧格式的单个 led 参数自动转为 LED 键值对。
     */
    if (obj.found_fields)
    {
        memcpy(ack->fields, obj.fields, sizeof(obj.fields));
        ack->field_count = obj.field_count;
    }
    else if (obj.found_led)
    {
        snprintf(ack->fields[0].key, sizeof(ack->fields[0].key), "LED");
        snprintf(ack->fields[0].value, sizeof(ack->fields[0].value), "%d", obj.led_value);
        ack->field_count = 1;
    }
    else
    {
        ack->field_count = 0;
    }

    return consumed;
}

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
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回-1
 */
int message_json_decode_nack(
    const char *line,
    size_t line_length,
    frame_nack_t *nack)
{
    json_object_t obj;
    int consumed;

    if (nack == NULL)
    {
        return -1;
    }

    /* 失败时输出保持干净 */
    memset(nack, 0, sizeof(*nack));

    consumed = json_parse_object(line, line_length, &obj);

    if (consumed < 0)
    {
        return -1;
    }

    /* 必需字段必须齐全 */
    if (!obj.found_node || !obj.found_seq ||
        !obj.found_error || !obj.found_type)
    {
        return -1;
    }

    /* 类型必须是NACK */
    if (strcmp(obj.type_value, "NACK") != 0)
    {
        return -1;
    }

    if (strlen(obj.node_id) >= sizeof(nack->node_id))
    {
        return -1;
    }

    strcpy(nack->node_id, obj.node_id);
    nack->sequence = obj.sequence;

    if (strlen(obj.error) >= sizeof(nack->error))
    {
        return -1;
    }

    strcpy(nack->error, obj.error);

    return consumed;
}

/*
 * 解析一个JSON数字（可带负号、小数），转换为放大10倍后的整数。
 *
 * 例："25.3" -> 253，"60" -> 600，"-3.5" -> -35。
 *
 * 小数只取第1位（对应x10语义），多余小数位忽略；
 * 指数部分（e/E）对协议无意义，这里不支持。
 *
 * @param pp      输入：指向数字起始位置；输出：数字结束后的位置
 * @param end     缓冲区结束位置（不含）
 * @param out_x10 输出放大10倍后的整数值
 *
 * @return 0成功，-1失败（不是合法数字或数值溢出）
 */
static int json_parse_x10(
    const char **pp,
    const char *end,
    int *out_x10)
{
    const char *p;
    int negative;
    unsigned long int_part;
    unsigned long frac_digit;
    int has_frac;

    p = *pp;
    negative = 0;
    int_part = 0;
    frac_digit = 0;
    has_frac = 0;

    if (p >= end)
    {
        return -1;
    }

    if (*p == '-')
    {
        negative = 1;
        p++;
    }

    if (p >= end || *p < '0' || *p > '9')
    {
        return -1;   /* 必须至少有1位整数 */
    }

    while (p < end && *p >= '0' && *p <= '9')
    {
        if (int_part > (0x7FFFFFFFUL - 9UL) / 10UL)
        {
            return -1;   /* 防止转long后溢出 */
        }

        int_part = int_part * 10UL + (unsigned long)(*p - '0');
        p++;
    }

    if (p < end && *p == '.')
    {
        p++;

        if (p < end && *p >= '0' && *p <= '9')
        {
            frac_digit = (unsigned long)(*p - '0');
            has_frac = 1;
            p++;
        }

        /* 忽略多余小数位 */
        while (p < end && *p >= '0' && *p <= '9')
        {
            p++;
        }
    }

    *pp = p;

    if (has_frac)
    {
        long value;

        value = (long)int_part * 10L + (long)frac_digit;

        if (negative)
        {
            value = -value;
        }

        *out_x10 = (int)value;
    }
    else
    {
        long value;

        value = (long)int_part * 10L;

        if (negative)
        {
            value = -value;
        }

        *out_x10 = (int)value;
    }

    return 0;
}

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
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回-1
 */
/*
 * 往 DATA 解析结果中追加一条键值对。
 *
 * @param data      目标结构体
 * @param key       key 文本
 * @param key_len   key 长度
 * @param value     value 文本
 * @param value_len value 长度
 *
 * @return 0成功，-1参数非法/超限/容量已满
 */
static int json_data_add_field(
    frame_data_t *data,
    const char *key,
    size_t key_len,
    const char *value,
    size_t value_len)
{
    if (data == NULL)
    {
        return -1;
    }

    return kv_array_add(
        data->fields,
        &data->field_count,
        key,
        key_len,
        value,
        value_len);
}

int message_json_decode_data(
    const char *line,
    size_t line_length,
    frame_data_t *data)
{
    const char *p;
    const char *end;
    int found_node;
    int found_seq;
    int found_type;
    uint32_t sequence;

    if (line == NULL ||
        line_length == 0U ||
        data == NULL)
    {
        return -1;
    }

    /* 失败时输出保持干净 */
    memset(data, 0, sizeof(*data));

    end = line + line_length;

    p = json_skip_whitespace(line, end);

    if (p >= end || *p != '{')
    {
        return -1;
    }

    p++;

    found_node = 0;
    found_seq = 0;
    found_type = 0;
    sequence = 0;

    for (;;)
    {
        const char *key_start;
        const char *key_end;
        int key_length;
        int is_node;
        int is_seq;
        int is_temp;
        int is_humi;
        int is_type;
        int is_fields;

        p = json_skip_whitespace(p, end);

        if (p >= end)
        {
            return -1;   /* JSON未闭合 */
        }

        if (*p == '}')
        {
            p++;
            break;       /* 对象结束 */
        }

        if (*p != '"')
        {
            return -1;   /* 键必须是字符串 */
        }

        /* 解析键名 */
        p++;
        key_start = p;

        while (p < end && *p != '"')
        {
            if (*p == '\\')
            {
                p++;
            }
            p++;
        }

        if (p >= end)
        {
            return -1;   /* 键字符串未闭合 */
        }

        key_end = p;
        p++;

        /* 期待 ':' */
        p = json_skip_whitespace(p, end);
        if (p >= end || *p != ':')
        {
            return -1;
        }
        p++;

        p = json_skip_whitespace(p, end);
        if (p >= end)
        {
            return -1;
        }

        key_length = (int)(key_end - key_start);
        is_node = (key_length == 4 &&
                   memcmp(key_start, "node", 4) == 0);
        is_seq = (key_length == 3 &&
                  memcmp(key_start, "seq", 3) == 0);
        is_temp = (key_length == 11 &&
                   memcmp(key_start, "temperature", 11) == 0);
        is_humi = (key_length == 8 &&
                   memcmp(key_start, "humidity", 8) == 0);
        is_type = (key_length == 4 &&
                   memcmp(key_start, "type", 4) == 0);
        is_fields = (key_length == 6 &&
                     memcmp(key_start, "fields", 6) == 0);

        if (is_fields)
        {
            /*
             * fields 值必须是对象：
             * "fields":{"T":"253","H":"601"}
             *
             * 每个键值对原样存入 data->fields；
             * 值既可以是字符串，也可以是数字（原文存入）。
             */
            const char *fkey_start;
            const char *fkey_end;
            size_t fkey_len;

            if (*p != '{')
            {
                return -1;
            }
            p++;

            for (;;)
            {
                p = json_skip_whitespace(p, end);

                if (p >= end)
                {
                    return -1;   /* fields 未闭合 */
                }

                if (*p == '}')
                {
                    p++;
                    break;       /* fields 对象结束 */
                }

                if (*p != '"')
                {
                    return -1;   /* 字段键必须是字符串 */
                }

                /* 解析字段键名 */
                p++;
                fkey_start = p;

                while (p < end && *p != '"')
                {
                    if (*p == '\\')
                    {
                        p++;
                    }
                    p++;
                }

                if (p >= end)
                {
                    return -1;
                }

                fkey_end = p;
                p++;

                /* 期待 ':' */
                p = json_skip_whitespace(p, end);
                if (p >= end || *p != ':')
                {
                    return -1;
                }
                p++;

                p = json_skip_whitespace(p, end);
                if (p >= end)
                {
                    return -1;
                }

                fkey_len = (size_t)(fkey_end - fkey_start);

                if (fkey_len == 0U ||
                    fkey_len >= FRAME_KV_KEY_MAX ||
                    data->field_count >= FRAME_DATA_MAX_FIELDS)
                {
                    return -1;
                }

                if (*p == '"')
                {
                    /* 字符串值 */
                    const char *value_start;
                    const char *value_end;
                    size_t value_length;

                    p++;
                    value_start = p;

                    while (p < end && *p != '"')
                    {
                        if (*p == '\\')
                        {
                            p++;
                        }
                        p++;
                    }

                    if (p >= end)
                    {
                        return -1;   /* 值字符串未闭合 */
                    }

                    value_end = p;
                    p++;

                    value_length = (size_t)(value_end - value_start);

                    if (json_data_add_field(
                            data,
                            fkey_start,
                            fkey_len,
                            value_start,
                            value_length) != 0)
                    {
                        return -1;
                    }
                }
                else
                {
                    /* 数字值：原文存入 value */
                    const char *value_start = p;
                    size_t value_length;

                    if (*p == '-')
                    {
                        p++;
                    }

                    if (p >= end || *p < '0' || *p > '9')
                    {
                        return -1;
                    }

                    while (p < end && *p >= '0' && *p <= '9')
                    {
                        p++;
                    }

                    if (p < end && *p == '.')
                    {
                        p++;
                        if (p >= end || *p < '0' || *p > '9')
                        {
                            return -1;
                        }
                        while (p < end && *p >= '0' && *p <= '9')
                        {
                            p++;
                        }
                    }

                    if (p < end && (*p == 'e' || *p == 'E'))
                    {
                        p++;
                        if (p < end && (*p == '+' || *p == '-'))
                        {
                            p++;
                        }
                        if (p >= end || *p < '0' || *p > '9')
                        {
                            return -1;
                        }
                        while (p < end && *p >= '0' && *p <= '9')
                        {
                            p++;
                        }
                    }

                    value_length = (size_t)(p - value_start);

                    if (json_data_add_field(
                            data,
                            fkey_start,
                            fkey_len,
                            value_start,
                            value_length) != 0)
                    {
                        return -1;
                    }
                }

                /* 期待 ',' 或 '}' */
                p = json_skip_whitespace(p, end);

                if (p >= end)
                {
                    return -1;
                }

                if (*p == ',')
                {
                    p++;
                }
                else if (*p == '}')
                {
                    p++;
                    break;
                }
                else
                {
                    return -1;
                }
            }
        }
        else if (*p == '"')
        {
            /* 字符串值 */
            const char *value_start;
            const char *value_end;
            size_t value_length;

            p++;
            value_start = p;

            while (p < end && *p != '"')
            {
                if (*p == '\\')
                {
                    p++;
                }
                p++;
            }

            if (p >= end)
            {
                return -1;   /* 值字符串未闭合 */
            }

            value_end = p;
            p++;

            value_length = (size_t)(value_end - value_start);

            if (is_node)
            {
                if (value_length >= sizeof(data->node_id))
                {
                    return -1;   /* 节点编号过长 */
                }

                memcpy(data->node_id, value_start, value_length);
                data->node_id[value_length] = '\0';
                found_node = 1;
            }
            else if (is_type)
            {
                /* 类型必须是DATA */
                if (value_length != 4U ||
                    memcmp(value_start, "DATA", 4) != 0)
                {
                    return -1;
                }
                found_type = 1;
            }
            /* 其它未知字符串字段：值已跳过，忽略 */
        }
        else
        {
            /* 数字值 */
            if (is_temp || is_humi)
            {
                int x10;
                char value_text[FRAME_KV_VALUE_MAX];
                int value_length;

                if (json_parse_x10(&p, end, &x10) != 0)
                {
                    return -1;
                }

                /*
                 * 旧格式兼容：
                 * temperature/humidity 放大10倍后转成 T/H 键值对。
                 */
                value_length = snprintf(
                    value_text,
                    sizeof(value_text),
                    "%d",
                    x10);

                if (value_length < 0 ||
                    (size_t)value_length >= sizeof(value_text) ||
                    json_data_add_field(
                        data,
                        is_temp ? "T" : "H",
                        1U,
                        value_text,
                        (size_t)value_length) != 0)
                {
                    return -1;
                }
            }
            else if (is_seq)
            {
                unsigned long value;

                value = 0;

                if (*p == '-')
                {
                    return -1;   /* 序号不允许为负 */
                }

                while (p < end && *p >= '0' && *p <= '9')
                {
                    unsigned long digit;

                    digit = (unsigned long)(*p - '0');

                    if (value > (0xFFFFFFFFUL - digit) / 10U)
                    {
                        return -1;   /* 超出uint32_t范围 */
                    }

                    value = value * 10U + digit;
                    p++;
                }

                /* 小数部分：只消费 */
                if (p < end && *p == '.')
                {
                    p++;
                    while (p < end && *p >= '0' && *p <= '9')
                    {
                        p++;
                    }
                }

                /* 指数部分：只消费 */
                if (p < end && (*p == 'e' || *p == 'E'))
                {
                    p++;
                    if (p < end && (*p == '+' || *p == '-'))
                    {
                        p++;
                    }
                    while (p < end && *p >= '0' && *p <= '9')
                    {
                        p++;
                    }
                }

                sequence = (uint32_t)value;
                found_seq = 1;
            }
            else
            {
                /* 未知数字字段：跳过数值 */
                if (*p == '-')
                {
                    p++;
                }

                while (p < end && *p >= '0' && *p <= '9')
                {
                    p++;
                }

                if (p < end && *p == '.')
                {
                    p++;
                    while (p < end && *p >= '0' && *p <= '9')
                    {
                        p++;
                    }
                }

                if (p < end && (*p == 'e' || *p == 'E'))
                {
                    p++;
                    if (p < end && (*p == '+' || *p == '-'))
                    {
                        p++;
                    }
                    while (p < end && *p >= '0' && *p <= '9')
                    {
                        p++;
                    }
                }
            }
        }

        /* 期待 ',' 或 '}' */
        p = json_skip_whitespace(p, end);

        if (p >= end)
        {
            return -1;
        }

        if (*p == ',')
        {
            p++;
        }
        else if (*p == '}')
        {
            p++;
            break;
        }
        else
        {
            return -1;
        }
    }

    /* 必需字段必须齐全 */
    if (!found_node ||
        !found_seq ||
        !found_type)
    {
        return -1;
    }

    data->sequence = sequence;

    return (int)(p - line);
}