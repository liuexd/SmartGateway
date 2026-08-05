#include "crc16.h"

uint16_t crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFU;
    size_t i;
    unsigned int bit;

    /*
     * 防止传入空指针。
     *
     * length 为 0 时，data 可以是 NULL；
     * length 不为 0 时，data 不能是 NULL。
     */
    if (data == NULL && length != 0U)
    {
        return 0U;
    }

    /*
    
     * 逐字节处理输入数据。
     */
    for (i = 0U; i < length; ++i)
    {
        /*
         * 将当前字节放到 CRC 的高 8 位，
         * 再与当前 CRC 进行异或。
         */
        crc ^= (uint16_t)data[i] << 8U;

        /*
         * 一个字节有 8 位，因此处理 8 次。
         */
        for (bit = 0U; bit < 8U; ++bit)
        {
            /*
             * 检查 CRC 最高位是否为 1。
             */
            if ((crc & 0x8000U) != 0U)
            {
                /*
                 * 最高位为 1：
                 * 左移一位后，与多项式 0x1021 异或。
                 */
                crc = (uint16_t)((crc << 1U) ^ 0x1021U);
            }
            else
            {
                /*
                 * 最高位为 0：
                 * 只需要左移一位。
                 */
                crc = (uint16_t)(crc << 1U);
            }
        }
    }

    return crc;
}