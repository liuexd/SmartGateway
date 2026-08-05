#include "frame.h"
#include "crc16.h"

#include <stdio.h>
#include <string.h>

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

int frame_build_data(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int temperature_x10,
    int humidity_x10)
{
    char crc_data[FRAME_MAX_LEN];
    int crc_data_len;
    int frame_len;
    uint16_t crc;

    /*
     * 第一步：检查传入参数。
     */
    if (out == NULL || out_size == 0U || node_id == NULL)
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
    crc_data_len = snprintf(
        crc_data,
        sizeof(crc_data),
        "1,%s,%06u,DATA,T=%d,H=%d",
        node_id,
        (unsigned int)sequence,
        temperature_x10,
        humidity_x10);

    /*
     * snprintf 返回“原本需要写入的字符数”，不包含 '\0'。
     */
    if (crc_data_len < 0)
    {
        return FRAME_ERR_PARAM;
    }

    if ((size_t)crc_data_len >= sizeof(crc_data))
    {
        return FRAME_ERR_TOO_LONG;
    }

    /*
     * 第三步：对 crc_data 中的有效字节计算 CRC16。
     */
    crc = crc16_ccitt_false(
        (const uint8_t *)crc_data,
        (size_t)crc_data_len);

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

int frame_build_command(
    char *out,
    size_t out_size,
    const char *sender,
    uint32_t sequence,
    int led_value)
{
    char crc_command[FRAME_MAX_LEN];
    int crc_command_len;
    int frame_len;
    uint16_t crc;

    /*
     * 第一步：检查传入参数。
     */
    if (out == NULL || out_size == 0U || sender == NULL)
    {
        return FRAME_ERR_PARAM;
    }

    /*
     * 防止失败时输出缓冲区中残留旧内容。
     */
    out[0] = '\0';

    /*
     * 发送方编号不能含有协议中的特殊字符。
     */
    if (!node_id_is_valid(sender))
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
     */
    crc_command_len = snprintf(
        crc_command,
        sizeof(crc_command),
        "1,%s,%06u,CMD,LED=%d",
        sender,
        (unsigned int)sequence,
        led_value);

    /*
     * snprintf 返回“原本需要写入的字符数”，不包含 '\0'。
     */
    if (crc_command_len < 0)
    {
        return FRAME_ERR_PARAM;
    }

    if ((size_t)crc_command_len >= sizeof(crc_command))
    {
        return FRAME_ERR_TOO_LONG;
    }

    /*
     * 第三步：对 crc_command 中的有效字节计算 CRC16。
     */
    crc = crc16_ccitt_false(
        (const uint8_t *)crc_command,
        (size_t)crc_command_len);

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
        crc_command,
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

int frame_build_ack(
    char *out,
    size_t out_size,
    const char *node_id,
    uint32_t sequence,
    int led_value)
{
    char crc_ack[FRAME_MAX_LEN];
    int crc_ack_len;
    int frame_len;
    uint16_t crc;

    /*
     * 第一步：检查传入参数。
     */
    if (out == NULL || out_size == 0U || node_id == NULL)
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
    crc_ack_len = snprintf(
        crc_ack,
        sizeof(crc_ack),
        "1,%s,%06u,ACK,LED=%d",
        node_id,
        (unsigned int)sequence,
        led_value);
    /*
     * snprintf 返回“原本需要写入的字符数”，不包含 '\0'。
     */
    if (crc_ack_len < 0)
    {
        return FRAME_ERR_PARAM;
    }

    if ((size_t)crc_ack_len >= sizeof(crc_ack))
    {
        return FRAME_ERR_TOO_LONG;
    }

    /*
     * 第三步：对 crc_data 中的有效字节计算 CRC16。
     */
    crc = crc16_ccitt_false(
        (const uint8_t *)crc_ack,
        (size_t)crc_ack_len);

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
        crc_ack,
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