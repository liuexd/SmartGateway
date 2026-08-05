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
    int json_length;

    if (out == NULL || out_size == 0U || data == NULL)
    {
        return -1;
    }

    out[0] = '\0';

    json_length = snprintf(
        out,
        out_size,
        "{\"node\":\"%s\","
        "\"seq\":%u,"
        "\"type\":\"DATA\","
        "\"temperature\":%.1f,"
        "\"humidity\":%.1f}\n",
        data->node_id,
        (unsigned int)data->sequence,
        data->temperature_x10 / 10.0,
        data->humidity_x10 / 10.0
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
 * 将ACK帧解析结果转换为JSON字符串。
 */
int message_json_build_ack(
    char *out,
    size_t out_size,
    const frame_ack_t *ack
)
{
    int json_length;

    if (out == NULL || out_size == 0U || ack == NULL)
    {
        return -1;
    }

    out[0] = '\0';

    json_length = snprintf(
        out,
        out_size,
        "{\"node\":\"%s\","
        "\"seq\":%u,"
        "\"type\":\"ACK\","
        "\"led\":%d}\n",
        ack->node_id,
        (unsigned int)ack->sequence,
        ack->led_value
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
    int json_length;

    if (out == NULL || out_size == 0U || command == NULL)
    {
        return -1;
    }

    out[0] = '\0';

    json_length = snprintf(
        out,
        out_size,
        "{\"node\":\"%s\","
        "\"seq\":%u,"
        "\"type\":\"CMD\","
        "\"led\":%d}\n",
        command->sender,
        (unsigned int)command->sequence,
        command->led_value
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
} json_object_t;

/*
 * 解析一个简单JSON对象，提取协议关心的字段。
 *
 * 支持：
 *   {"node":"NODE01","seq":1,"type":"ACK","led":1}
 *   {"node":"NODE01","seq":1,"type":"NACK","error":"bad_cmd"}
 *
 * "led":"1" 这种字符串形式的数字也兼容；
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
 * 输入格式与 message_json_build_command() 的生成格式一致：
 * {"node":"GATEWAY","seq":1,"type":"CMD","led":1}
 *
 * 为兼容起见，也接受 "led":"1" 这种字符串形式的参数。
 * 其它无关字段（如温度、湿度）会被忽略；
 * 若 "type" 字段存在但取值不是 "CMD"，则解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param command     解析结果输出（成功时填充sender/sequence/led_value，
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
        !obj.found_led || !obj.found_type)
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
    command->led_value = obj.led_value;

    return consumed;
}

/*
 * 将JSON字符串转换为ACK帧解析结果。
 *
 * 输入格式与 message_json_build_ack() 的生成格式一致：
 * {"node":"NODE01","seq":1,"type":"ACK","led":1}
 *
 * 兼容 "led":"1" 这种字符串形式的参数；
 * 无关字段会被忽略，缺失必需字段或type不是"ACK"时解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param ack         解析结果输出（成功时填充node_id/sequence/led_value，
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
        !obj.found_led || !obj.found_type)
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
    ack->led_value = obj.led_value;

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
 * 输入格式与 message_json_build_data() 的生成格式一致：
 * {"node":"NODE01","seq":1,"type":"DATA","temperature":25.3,"humidity":60.1}
 *
 * temperature/humidity 支持带小数的数字，解析结果放大10倍
 * （25.3 -> 253，60.1 -> 601），与 frame_data_t 的语义一致。
 * 无关字段会被忽略，缺失必需字段或type不是"DATA"时解析失败。
 *
 * @param line        JSON文本（可带结尾的\r\n）
 * @param line_length JSON文本长度
 * @param data        解析结果输出（成功时填充node_id/sequence/
 *                    temperature_x10/humidity_x10，失败时清零）
 *
 * @return 成功返回已消费的字节数（不含结尾'\0'），失败返回-1
 */
int message_json_decode_data(
    const char *line,
    size_t line_length,
    frame_data_t *data)
{
    const char *p;
    const char *end;
    int found_node;
    int found_seq;
    int found_temp;
    int found_humi;
    int found_type;
    uint32_t sequence;
    int temperature_x10;
    int humidity_x10;

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
    found_temp = 0;
    found_humi = 0;
    found_type = 0;
    sequence = 0;
    temperature_x10 = 0;
    humidity_x10 = 0;

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

                if (json_parse_x10(&p, end, &x10) != 0)
                {
                    return -1;
                }

                if (is_temp)
                {
                    temperature_x10 = x10;
                    found_temp = 1;
                }
                else
                {
                    humidity_x10 = x10;
                    found_humi = 1;
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
        !found_temp ||
        !found_humi ||
        !found_type)
    {
        return -1;
    }

    data->sequence = sequence;
    data->temperature_x10 = temperature_x10;
    data->humidity_x10 = humidity_x10;

    return (int)(p - line);
}