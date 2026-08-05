#include "crc16.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void test_standard_data(void)
{
    const char text[] = "123456789";

    uint16_t crc = crc16_ccitt_false(
        (const uint8_t *)text,
        strlen(text)
    );

    printf(
        "standard data: \"%s\"\n"
        "calculated CRC: 0x%04X\n"
        "expected CRC:   0x29B1\n\n",
        text,
        (unsigned int)crc
    );

    /*
     * CRC-16/CCITT-FALSE 的标准测试结果。
     */
    assert(crc == 0x29B1U);
}

static void test_protocol_body(void)
{
    /*
     * 注意：
     * 这里只计算 @ 后、* 前的内容。
     *
     * 不包括：
     * @
     * *
     * CRC 字符
     * \r\n
     */
    const char body[] =
        "1,NODE01,000001,DATA,T=253,H=601";

    uint16_t crc = crc16_ccitt_false(
        (const uint8_t *)body,
        strlen(body)
    );

    printf(
        "protocol body: \"%s\"\n"
        "calculated CRC: 0x%04X\n"
        "expected CRC:   0x3F2E\n\n",
        body,
        (unsigned int)crc
    );

    assert(crc == 0x3F2EU);
}

static void test_empty_data(void)
{
    uint16_t crc = crc16_ccitt_false(NULL, 0U);

    printf(
        "empty data CRC: 0x%04X\n"
        "expected CRC:   0xFFFF\n\n",
        (unsigned int)crc
    );

    /*
     * 没有处理任何字节时，CRC 保持初始值。
     */
    assert(crc == 0xFFFFU);
}

static void test_changed_data(void)
{
    const char original[] =
        "1,NODE01,000001,DATA,T=253,H=601";

    const char changed[] =
        "1,NODE01,000001,DATA,T=283,H=601";

    uint16_t original_crc = crc16_ccitt_false(
        (const uint8_t *)original,
        strlen(original)
    );

    uint16_t changed_crc = crc16_ccitt_false(
        (const uint8_t *)changed,
        strlen(changed)
    );

    printf(
        "original CRC: 0x%04X\n"
        "changed CRC:  0x%04X\n\n",
        (unsigned int)original_crc,
        (unsigned int)changed_crc
    );

    /*
     * 数据发生变化后，CRC 一般也应该发生变化。
     */
    assert(original_crc != changed_crc);
}

int main(void)
{
    test_standard_data();
    test_protocol_body();
    test_empty_data();
    test_changed_data();

    printf("All CRC16 tests passed.\n");

    return 0;
}