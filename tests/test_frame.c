#include "frame.h"

#include <stdio.h>
#include <string.h>

static int test_normal_frame(void)
{
    char frame[FRAME_MAX_LEN];
    const char *expected =
        "@1,NODE01,000001,DATA,T=253,H=601*3F2E\r\n";

    int frame_len = frame_build_data(
        frame,
        sizeof(frame),
        "NODE01",
        1,
        253,
        601
    );

    if (frame_len < 0)
    {
        printf("[FAIL] frame_build_data returned %d\n", frame_len);
        return -1;
    }

    printf("Generated frame:\n%s", frame);

    if (strcmp(frame, expected) != 0)
    {
        printf("[FAIL] Generated frame is incorrect\n");
        printf("Expected: %s", expected);
        printf("Actual:   %s", frame);
        return -1;
    }

    if ((size_t)frame_len != strlen(frame))
    {
        printf("[FAIL] Returned length is incorrect\n");
        return -1;
    }

    printf("[PASS] Normal frame test\n");
    return 0;
}

static int test_negative_temperature(void)
{
    char frame[FRAME_MAX_LEN];

    int frame_len = frame_build_data(
        frame,
        sizeof(frame),
        "NODE01",
        2,
        -53,
        601
    );

    if (frame_len < 0)
    {
        printf("[FAIL] Negative temperature frame\n");
        return -1;
    }

    if (strstr(frame, "T=-53") == NULL)
    {
        printf("[FAIL] Negative temperature was not encoded correctly\n");
        return -1;
    }

    printf("[PASS] Negative temperature test\n");
    return 0;
}

static int test_sequence_overflow(void)
{
    char frame[FRAME_MAX_LEN];

    int frame_len = frame_build_data(
        frame,
        sizeof(frame),
        "NODE01",
        1000000,
        250,
        500
    );

    if (frame_len < 0)
    {
        printf("[FAIL] Sequence overflow frame\n");
        return -1;
    }

    /*
     * 1000000 % 1000000 = 0，
     * 因此序号应重新变成 000000。
     */
    if (strstr(frame, ",000000,DATA,") == NULL)
    {
        printf("[FAIL] Sequence did not wrap to zero\n");
        return -1;
    }

    printf("[PASS] Sequence overflow test\n");
    return 0;
}

static int test_small_buffer(void)
{
    char frame[10];

    int frame_len = frame_build_data(
        frame,
        sizeof(frame),
        "NODE01",
        1,
        253,
        601
    );

    if (frame_len != FRAME_ERR_TOO_LONG)
    {
        printf("[FAIL] Small buffer was not detected\n");
        return -1;
    }

    printf("[PASS] Small buffer test\n");
    return 0;
}

static int test_invalid_node_id(void)
{
    char frame[FRAME_MAX_LEN];

    /*
     * 节点编号包含逗号，会破坏协议字段划分。
     */
    int frame_len = frame_build_data(
        frame,
        sizeof(frame),
        "NODE,01",
        1,
        253,
        601
    );

    if (frame_len != FRAME_ERR_NODE_ID)
    {
        printf("[FAIL] Invalid node ID was not detected\n");
        return -1;
    }

    printf("[PASS] Invalid node ID test\n");
    return 0;
}

/*
 * 设备类型字典解析测试。
 */
static int test_device_type_dict(void)
{
    if (frame_device_type_from_text("1") != FRAME_DEVICE_THSENSOR)
    {
        printf("[FAIL] dev 1 should be THSENSOR\n");
        return -1;
    }

    if (frame_device_type_from_text("2") != FRAME_DEVICE_RELAY)
    {
        printf("[FAIL] dev 2 should be RELAY\n");
        return -1;
    }

    if (frame_device_type_from_text("3") != FRAME_DEVICE_MOTOR)
    {
        printf("[FAIL] dev 3 should be MOTOR\n");
        return -1;
    }

    if (frame_device_type_from_text("99") != FRAME_DEVICE_UNKNOWN ||
        frame_device_type_from_text(NULL) != FRAME_DEVICE_UNKNOWN ||
        frame_device_type_from_text("") != FRAME_DEVICE_UNKNOWN)
    {
        printf("[FAIL] unknown dev should be UNKNOWN\n");
        return -1;
    }

    if (strcmp(frame_device_type_name(FRAME_DEVICE_THSENSOR), "THSENSOR") != 0 ||
        strcmp(frame_device_type_name(FRAME_DEVICE_RELAY), "RELAY") != 0 ||
        strcmp(frame_device_type_name(FRAME_DEVICE_MOTOR), "MOTOR") != 0 ||
        strcmp(frame_device_type_name(FRAME_DEVICE_UNKNOWN), "UNKNOWN") != 0)
    {
        printf("[FAIL] device type name mismatch\n");
        return -1;
    }

    printf("[PASS] Device type dict test\n");
    return 0;
}

int main(void)
{
    int failed_count = 0;

    if (test_normal_frame() != 0)
    {
        failed_count++;
    }

    if (test_negative_temperature() != 0)
    {
        failed_count++;
    }

    if (test_sequence_overflow() != 0)
    {
        failed_count++;
    }

    if (test_small_buffer() != 0)
    {
        failed_count++;
    }

    if (test_invalid_node_id() != 0)
    {
        failed_count++;
    }

    if (test_device_type_dict() != 0)
    {
        failed_count++;
    }

    if (failed_count == 0)
    {
        printf("\nAll frame tests passed.\n");
        return 0;
    }

    printf("\n%d frame test(s) failed.\n", failed_count);
    return 1;
}