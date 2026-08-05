#ifndef CRC16_H
#define CRC16_H

#include <stddef.h>
#include <stdint.h>

/*
 * 计算 CRC-16/CCITT-FALSE。
 *
 * 参数：
 * data   指向待校验字节数组
 * length 待校验数据的字节数
 *
 * 返回值：
 * 16 位 CRC 校验结果
 */
uint16_t crc16_ccitt_false(const uint8_t *data, size_t length);

#endif