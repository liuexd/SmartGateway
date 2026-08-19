#include "frame.h"
#include "crc16.h"

#include <stdio.h>
#include <string.h>

frame_device_type_t frame_device_type_from_text(
    const char *text)
{
    if (text == NULL)
    {
        return FRAME_DEVICE_UNKNOWN;
    }

    if (strcmp(text, "1") == 0)
    {
        return FRAME_DEVICE_THSENSOR;
    }

    if (strcmp(text, "2") == 0)
    {
        return FRAME_DEVICE_RELAY;
    }

    if (strcmp(text, "3") == 0)
    {
        return FRAME_DEVICE_MOTOR;
    }

    return FRAME_DEVICE_UNKNOWN;
}

const char *frame_device_type_name(
    frame_device_type_t type)
{
    switch (type)
    {
        case FRAME_DEVICE_THSENSOR:
            return "THSENSOR";

        case FRAME_DEVICE_RELAY:
            return "RELAY";

        case FRAME_DEVICE_MOTOR:
            return "MOTOR";

        case FRAME_DEVICE_UNKNOWN:
        default:
            return "UNKNOWN";
    }
}

/**
 * @brief 检查节点编号中是否存在协议保留字符
 */
static int node_id_is_valid(const char *node_id)
{
    size_t i;

    if (node_id == NULL || node_id[0] == '\0')
    {
        return 0;
    }

    for (i = 0; node_id[i] != '\0'; i++)
    {
        switch (node_id[i])
        {
        case '@':
        case '*':
        case ',':
        case '\r':
        case '\n':
            return 0;

        default:
            break;
        }
    }

    return 1;
}

/*
 * 检查字段文本（key 或 value）中是否存在协议保留字符。
 *
 * 与 node_id_is_valid 的区别：
 * 键值对中额外禁止 '='，避免字段边界被破坏。
 */
static int field_text_is_valid(const char *text)
{
    size_t i;

    if (text == NULL || text[0] == '\0')
    {
        return 0;
    }

    for (i = 0; text[i] != '\0'; i++)
    {
        switch (text[i])
        {
        case '@':
        case '*':
        case ',':
        case '=':
        case '\r':
        case '\n':
            return 0;

        default:
            break;
        }
    }

    return 1;
}

/*
 * 生成一帧通用 KV 帧（内部共用）。
 *
 * DATA / CMD / ACK 三种帧的固定头部都是：
 * 1,<id>,<sequence>,<TYPE>
 *
 * 只是 id 字段语义（节点号/发送方）、类型字段不同，
 * 组帧流程完全一致，因此提取为同一个函数。
 *
 * @param out         输出缓冲区
 * @param out_size    输出缓冲区大小
 * @param id          节点编号或发送方标识
 * @param sequence    帧序号
 * @param type        "DATA" / "CMD" / "ACK"
 * @param fields      键值对数组
 * @param field_count 键值对数量
 *
 * @return 成功时返回帧的实际长度（不含结尾 '\0'），失败返回负数错误码
 */
static int build_kv_frame(
    char *out,
    size_t out_size,
    const char *id,
    uint32_t sequence,
    const char *type,
    const frame_kv_t *fields,
    size_t field_count)
{
    char crc_data[FRAME_MAX_LEN];
    size_t pos;
    size_t i;
    int frame_len;
    uint16_t crc;

    /*
     * 第一步：检查传入参数。
     */
    if (out == NULL || out_size == 0U ||
        id == NULL || type == NULL)
    {
        return FRAME_ERR_PARAM;
    }

    if (fields == NULL && field_count > 0U)
    {
        return FRAME_ERR_PARAM;
    }

    if (field_count > FRAME_DATA_MAX_FIELDS)
    {
        return FRAME_ERR_PARAM;
    }

    /*
     * 防止失败时输出缓冲区中残留旧内容。
     */
    out[0] = '\0';

    /*
     * 节点编号/发送方不能含有协议中的特殊字符。
     */
    if (!node_id_is_valid(id))
    {
        return FRAME_ERR_NODE_ID;
    }

    /*
     * 协议序号固定为 6 位。
     * 当序号超过 999999 时，从 0 重新开始。
     */
    sequence %= 1000000U;

    /*
     * 第二步：先生成参与 CRC 计算的数据。
     *
     * 注意：
     * 不包含帧头 @
     * 不包含分隔符 *
     * 不包含 CRC
     * 不包含 \r\n
     *
     * 固定头部为：
     * 1,NODE01,000001,DATA
     */
    pos = (size_t)snprintf(
        crc_data,
        sizeof(crc_data),
        "1,%s,%06u,%s",
        id,
        (unsigned int)sequence,
        type);

    if (pos >= sizeof(crc_data))
    {
        return FRAME_ERR_TOO_LONG;
    }

    /*
     * 第三步：逐个拼接键值对字段。
     *
     * 每个字段形如 ,KEY=VALUE：
     * key/value 必须非空，且不能含有协议保留字符。
     */
    for (i = 0; i < field_count; i++)
    {
        const char *key = fields[i].key;
        const char *value = fields[i].value;
        int written;

        if (key == NULL ||
            value == NULL ||
            key[0] == '\0' ||
            value[0] == '\0' ||
            !field_text_is_valid(key) ||
            !field_text_is_valid(value))
        {
            return FRAME_ERR_PARAM;
        }

        /*
         * pos 是已写入字符数，必须小于缓冲区大小，
         * 否则无法容纳结尾 '\0'。
         */
        if (pos >= sizeof(crc_data))
        {
            return FRAME_ERR_TOO_LONG;
        }

        written = snprintf(
            crc_data + pos,
            sizeof(crc_data) - pos,
            ",%s=%s",
            key,
            value);

        if (written < 0)
        {
            return FRAME_ERR_PARAM;
        }

        if ((size_t)written >= sizeof(crc_data) - pos)
        {
            return FRAME_ERR_TOO_LONG;
        }

        pos += (size_t)written;
    }

    /*
     * 第四步：对 crc_data 中的有效字节计算 CRC16。
     */
    crc = crc16_ccitt_false(
        (const uint8_t *)crc_data,
        (size_t)pos);

    /*
     * 第五步：拼接完整帧。
     *
     * %04X 表示：
     * 使用十六进制大写输出；
     * 不足 4 位时在前面补 0。
     */
    frame_len = snprintf(
        out,
        out_size,
        "@%s*%04X\r\n",
        crc_data,
        (unsigned int)crc);

    if (frame_len < 0)
    {
        out[0] = '\0';
        return FRAME_ERR_PARAM;
    }

    /*
     * frame_len 不包含 '\0'。
     * 因此必须保证 frame_len < out_size，
     * 才能容纳字符串末尾的 '\0'。
     */
    if ((size_t)frame_len >= out_size)
    {
        out[0] = '\0';
        return FRAME_ERR_TOO_LONG;
    }

    return frame_len;
}

int frame_build_data_kv(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    const frame_kv_t *fields,
    size_t field_count)
{
    return build_kv_frame(
        out,
        out_size,
        node_id,
        sequence,
        "DATA",
        fields,
        field_count);
}

int frame_build_data(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int temperature_x10,
    int humidity_x10)
{
    frame_kv_t fields[2];

    /*
     * 旧接口的便捷封装：
     * 把 T/H 两个参数转换成两条键值对，交给通用组帧函数。
     *
     * snprintf 的结果必然小于 FRAME_KV_KEY_MAX/FRAME_KV_VALUE_MAX，
     * 因为帧内已对长度有硬约束（FRAME_MAX_LEN），无需再次检查。
     */
    snprintf(fields[0].key, sizeof(fields[0].key), "T");
    snprintf(fields[0].value, sizeof(fields[0].value), "%d", temperature_x10);

    snprintf(fields[1].key, sizeof(fields[1].key), "H");
    snprintf(fields[1].value, sizeof(fields[1].value), "%d", humidity_x10);

    return frame_build_data_kv(
        out,
        out_size,
        node_id,
        sequence,
        fields,
        2U);
}

int frame_build_command_kv(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    const frame_kv_t *fields,
    size_t field_count)
{
    return build_kv_frame(
        out,
        out_size,
        sender,
        sequence,
        "CMD",
        fields,
        field_count);
}

int frame_build_command(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    int led_value)
{
    frame_kv_t fields[1];

    /*
     * 旧接口的便捷封装：
     * 把 LED 参数转换成一条键值对，交给通用组帧函数。
     */
    snprintf(fields[0].key, sizeof(fields[0].key), "LED");
    snprintf(fields[0].value, sizeof(fields[0].value), "%d", led_value);

    return frame_build_command_kv(
        out,
        out_size,
        sender,
        sequence,
        fields,
        1U);
}

int frame_build_ack_kv(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    const frame_kv_t *fields,
    size_t field_count)
{
    return build_kv_frame(
        out,
        out_size,
        node_id,
        sequence,
        "ACK",
        fields,
        field_count);
}

int frame_build_ack(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int led_value)
{
    frame_kv_t fields[1];

    /*
     * 旧接口的便捷封装：
     * 把 LED 回显转换成一条键值对，交给通用组帧函数。
     */
    snprintf(fields[0].key, sizeof(fields[0].key), "LED");
    snprintf(fields[0].value, sizeof(fields[0].value), "%d", led_value);

    return frame_build_ack_kv(
        out,
        out_size,
        node_id,
        sequence,
        fields,
        1U);
}

int frame_build_nack(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    const char *error)
{
    char crc_nack[FRAME_MAX_LEN];
    int crc_nack_len;
    int frame_len;
    uint16_t crc;

    /*
     * 第一步：检查传入参数。
     */
    if (out == NULL || out_size == 0U ||
        node_id == NULL || error == NULL)
    {
        return FRAME_ERR_PARAM;
    }

    /*
     * 防止失败时输出缓冲区中残留旧内容。
     */
    out[0] = '\0';

    /*
     * 节点编号不能含有协议中的特殊字符。
     */
    if (!node_id_is_valid(node_id))
    {
        return FRAME_ERR_NODE_ID;
    }

    /*
     * 错误描述也不能含有协议中的特殊字符。
     *
     * 否则会破坏帧结构：
     * 逗号会让 payload 字段错位；
     * @、*、\r、\n 会干扰帧头和CRC分隔符的定位。
     */
    if (!node_id_is_valid(error))
    {
        return FRAME_ERR_PARAM;
    }

    /*
     * 协议序号固定为 6 位。
     * 当序号超过 999999 时，从 0 重新开始。
     */
    sequence %= 1000000U;

    /*
     * 第二步：先生成参与 CRC 计算的数据。
     *
     * 注意：
     * 不包含帧头 @
     * 不包含分隔符 *
     * 不包含 CRC
     * 不包含 \r\n
     */
    crc_nack_len = snprintf(
        crc_nack,
        sizeof(crc_nack),
        "1,%s,%06u,NACK,ERR=%s",
        node_id,
        (unsigned int)sequence,
        error);

    /*
     * snprintf 返回“原本需要写入的字符数”，不包含 '\0'。
     */
    if (crc_nack_len < 0)
    {
        return FRAME_ERR_PARAM;
    }

    if ((size_t)crc_nack_len >= sizeof(crc_nack))
    {
        return FRAME_ERR_TOO_LONG;
    }

    /*
     * 第三步：对 crc_nack 中的有效字节计算 CRC16。
     */
    crc = crc16_ccitt_false(
        (const uint8_t *)crc_nack,
        (size_t)crc_nack_len);

    /*
     * 第四步：拼接完整帧。
     *
     * %04X 表示：
     * 使用十六进制大写输出；
     * 不足 4 位时在前面补 0。
     */
    frame_len = snprintf(
        out,
        out_size,
        "@%s*%04X\r\n",
        crc_nack,
        (unsigned int)crc);

    if (frame_len < 0)
    {
        out[0] = '\0';
        return FRAME_ERR_PARAM;
    }

    /*
     * frame_len 不包含 '\0'。
     * 因此必须保证 frame_len < out_size，
     * 才能容纳字符串末尾的 '\0'。
     */
    if ((size_t)frame_len >= out_size)
    {
        out[0] = '\0';
        return FRAME_ERR_TOO_LONG;
    }

    return frame_len;
}