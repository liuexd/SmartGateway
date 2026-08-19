#include "frame.h"
#include "frame_parser.h"

#include <stdio.h>
#include <string.h>

/*
 * 回调函数使用的测试上下文。
 */
typedef struct
{
    size_t callback_count;

    parsed_frame_t last_frame;
    frame_data_t last_data;

    int decode_failures;
} test_context_t;

/*
 * 每当解析器发现合法帧，就执行一次该函数。
 */
static void on_frame(
    const parsed_frame_t *frame,
    void *user_data
)
{
    test_context_t *context;

    context = (test_context_t *)user_data;

    context->callback_count++;

    /*
     * 保存最后一帧，方便测试检查。
     */
    context->last_frame = *frame;

    /*
     * 进一步解析NODE、序号、数据字段。
     */
    if (frame_decode_data(
            frame,
            &context->last_data
        ) != 0)
    {
        context->decode_failures++;
    }
}

static void reset_context(test_context_t *context)
{
    memset(context, 0, sizeof(*context));
}

/*
 * 生成测试使用的标准数据帧。
 */
static int build_test_frame(
    char *frame,
    size_t size,
    uint32_t sequence
)
{
    return frame_build_data(
        frame,
        size,
        "NODE01",
        sequence,
        253,
        601
    );
}

/*
 * 测试1：一次输入完整合法帧。
 */
static int test_complete_frame(void)
{
    frame_parser_t parser;
    test_context_t context;

    char frame[FRAME_MAX_LEN];
    int length;

    frame_parser_init(&parser);
    reset_context(&context);

    length = build_test_frame(
        frame,
        sizeof(frame),
        1
    );

    if (length < 0)
    {
        return -1;
    }

    frame_parser_feed(
        &parser,
        (const uint8_t *)frame,
        (size_t)length,
        on_frame,
        &context
    );

    if (context.callback_count != 1U)
    {
        return -1;
    }

    if (parser.stats.valid_frames != 1U)
    {
        return -1;
    }

    if (context.decode_failures != 0)
    {
        return -1;
    }

    if (strcmp(
            context.last_data.node_id,
            "NODE01"
        ) != 0)
    {
        return -1;
    }

    if (context.last_data.sequence != 1U)
    {
        return -1;
    }

    if (context.last_data.field_count != 2U ||
        strcmp(frame_data_find_field(&context.last_data, "T"), "253") != 0 ||
        strcmp(frame_data_find_field(&context.last_data, "H"), "601") != 0)
    {
        return -1;
    }

    return 0;
}

/*
 * 测试2：每次只输入1个字节。
 *
 * 用来模拟串口严重半包情况。
 */
static int test_one_byte_at_a_time(void)
{
    frame_parser_t parser;
    test_context_t context;

    char frame[FRAME_MAX_LEN];
    int length;
    int i;

    frame_parser_init(&parser);
    reset_context(&context);

    length = build_test_frame(
        frame,
        sizeof(frame),
        2
    );

    if (length < 0)
    {
        return -1;
    }

    for (i = 0; i < length; i++)
    {
        frame_parser_feed(
            &parser,
            (const uint8_t *)&frame[i],
            1,
            on_frame,
            &context
        );
    }

    if (context.callback_count != 1U)
    {
        return -1;
    }

    if (context.last_data.sequence != 2U)
    {
        return -1;
    }

    return 0;
}

/*
 * 测试3：一次输入两个连续帧。
 *
 * 用来模拟粘包。
 */
static int test_two_frames_in_one_feed(void)
{
    frame_parser_t parser;
    test_context_t context;

    char frame1[FRAME_MAX_LEN];
    char frame2[FRAME_MAX_LEN];
    char combined[FRAME_MAX_LEN * 2];

    int length1;
    int length2;

    frame_parser_init(&parser);
    reset_context(&context);

    length1 = build_test_frame(
        frame1,
        sizeof(frame1),
        3
    );

    length2 = build_test_frame(
        frame2,
        sizeof(frame2),
        4
    );

    if (length1 < 0 || length2 < 0)
    {
        return -1;
    }

    memcpy(
        combined,
        frame1,
        (size_t)length1
    );

    memcpy(
        combined + length1,
        frame2,
        (size_t)length2
    );

    frame_parser_feed(
        &parser,
        (const uint8_t *)combined,
        (size_t)(length1 + length2),
        on_frame,
        &context
    );

    if (context.callback_count != 2U)
    {
        return -1;
    }

    /*
     * 最后一帧应该是序号4。
     */
    if (context.last_data.sequence != 4U)
    {
        return -1;
    }

    return 0;
}

/*
 * 测试4：先输入前半帧，再输入后半帧。
 */
static int test_half_frame(void)
{
    frame_parser_t parser;
    test_context_t context;

    char frame[FRAME_MAX_LEN];
    int length;
    size_t half;

    frame_parser_init(&parser);
    reset_context(&context);

    length = build_test_frame(
        frame,
        sizeof(frame),
        5
    );

    if (length < 0)
    {
        return -1;
    }

    half = (size_t)length / 2U;

    /*
     * 第一次只输入前半帧。
     */
    frame_parser_feed(
        &parser,
        (const uint8_t *)frame,
        half,
        on_frame,
        &context
    );

    /*
     * 还没收到\n，不应该输出帧。
     */
    if (context.callback_count != 0U)
    {
        return -1;
    }

    /*
     * 第二次输入剩余部分。
     */
    frame_parser_feed(
        &parser,
        (const uint8_t *)frame + half,
        (size_t)length - half,
        on_frame,
        &context
    );

    if (context.callback_count != 1U)
    {
        return -1;
    }

    if (context.last_data.sequence != 5U)
    {
        return -1;
    }

    return 0;
}

/*
 * 测试5：错误CRC后紧跟合法帧。
 */
static int test_bad_crc_then_valid_frame(void)
{
    frame_parser_t parser;
    test_context_t context;

    char bad_frame[FRAME_MAX_LEN];
    char good_frame[FRAME_MAX_LEN];
    char combined[FRAME_MAX_LEN * 2];

    int bad_length;
    int good_length;

    char *star;

    frame_parser_init(&parser);
    reset_context(&context);

    bad_length = build_test_frame(
        bad_frame,
        sizeof(bad_frame),
        6
    );

    good_length = build_test_frame(
        good_frame,
        sizeof(good_frame),
        7
    );

    if (bad_length < 0 || good_length < 0)
    {
        return -1;
    }

    /*
     * 找到*后面的第一位CRC字符，
     * 人为改错一位。
     */
    star = strchr(bad_frame, '*');

    if (star == NULL)
    {
        return -1;
    }

    if (star[1] == '0')
    {
        star[1] = '1';
    }
    else
    {
        star[1] = '0';
    }

    memcpy(
        combined,
        bad_frame,
        (size_t)bad_length
    );

    memcpy(
        combined + bad_length,
        good_frame,
        (size_t)good_length
    );

    frame_parser_feed(
        &parser,
        (const uint8_t *)combined,
        (size_t)(bad_length + good_length),
        on_frame,
        &context
    );

    /*
     * 第一帧CRC错误。
     */
    if (parser.stats.crc_errors != 1U)
    {
        return -1;
    }

    /*
     * 错误帧不能触发回调，
     * 只有后面的合法帧触发回调。
     */
    if (context.callback_count != 1U)
    {
        return -1;
    }

    if (context.last_data.sequence != 7U)
    {
        return -1;
    }

    return 0;
}

/*
 * 测试6：帧头前有乱码。
 */
static int test_garbage_before_frame(void)
{
    frame_parser_t parser;
    test_context_t context;

    const char garbage[] = "abc123\r\n";

    char frame[FRAME_MAX_LEN];
    char input[FRAME_MAX_LEN + sizeof(garbage)];

    int frame_length;

    frame_parser_init(&parser);
    reset_context(&context);

    frame_length = build_test_frame(
        frame,
        sizeof(frame),
        8
    );

    if (frame_length < 0)
    {
        return -1;
    }

    memcpy(
        input,
        garbage,
        sizeof(garbage) - 1U
    );

    memcpy(
        input + sizeof(garbage) - 1U,
        frame,
        (size_t)frame_length
    );

    frame_parser_feed(
        &parser,
        (const uint8_t *)input,
        sizeof(garbage) - 1U +
            (size_t)frame_length,
        on_frame,
        &context
    );

    /*
     * @之前的字符应该全部被丢弃。
     */
    if (parser.stats.discarded_bytes !=
        sizeof(garbage) - 1U)
    {
        return -1;
    }

    if (context.callback_count != 1U)
    {
        return -1;
    }

    return 0;
}

/*
 * 测试7：300字节超长帧后紧跟合法帧。
 */
static int test_overlong_frame_then_valid(void)
{
    frame_parser_t parser;
    test_context_t context;

    char long_input[300];
    char frame[FRAME_MAX_LEN];

    int frame_length;

    frame_parser_init(&parser);
    reset_context(&context);

    /*
     * 构造一个以@开始但没有结束符的超长帧。
     */
    memset(long_input, 'A', sizeof(long_input));
    long_input[0] = '@';

    frame_parser_feed(
        &parser,
        (const uint8_t *)long_input,
        sizeof(long_input),
        on_frame,
        &context
    );

    /*
     * 超长帧后再输入一个正常帧，
     * 检查解析器能否恢复。
     */
    frame_length = build_test_frame(
        frame,
        sizeof(frame),
        9
    );

    if (frame_length < 0)
    {
        return -1;
    }

    frame_parser_feed(
        &parser,
        (const uint8_t *)frame,
        (size_t)frame_length,
        on_frame,
        &context
    );

    if (parser.stats.overflow_errors != 1U)
    {
        return -1;
    }

    if (context.callback_count != 1U)
    {
        return -1;
    }

    if (context.last_data.sequence != 9U)
    {
        return -1;
    }

    return 0;
}

/*
 * 统一运行一个测试函数并打印结果。
 */
static int run_test(
    const char *name,
    int (*test_function)(void)
)
{
    if (test_function() == 0)
    {
        printf("[PASS] %s\n", name);
        return 0;
    }

    printf("[FAIL] %s\n", name);

    return 1;
}

int main(void)
{
    int failures = 0;

    failures += run_test(
        "complete frame",
        test_complete_frame
    );

    failures += run_test(
        "one byte at a time",
        test_one_byte_at_a_time
    );

    failures += run_test(
        "two frames in one feed",
        test_two_frames_in_one_feed
    );

    failures += run_test(
        "half frame",
        test_half_frame
    );

    failures += run_test(
        "bad CRC then valid frame",
        test_bad_crc_then_valid_frame
    );

    failures += run_test(
        "garbage before frame",
        test_garbage_before_frame
    );

    failures += run_test(
        "overlong frame then valid",
        test_overlong_frame_then_valid
    );

    if (failures == 0)
    {
        printf("\nAll parser tests passed.\n");
        return 0;
    }

    printf(
        "\n%d parser test(s) failed.\n",
        failures
    );

    return 1;
}