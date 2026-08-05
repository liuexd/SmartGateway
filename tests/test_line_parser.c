#include "line_parser.h"

#include <stdio.h>
#include <string.h>

/*
 * 测试上下文：记录回调收到的行数和内容。
 */
typedef struct
{
    int line_count;
    char last_line[256];
    size_t last_length;
} test_context_t;

static void on_line(
    const char *line,
    size_t length,
    void *user_data
)
{
    test_context_t *context = (test_context_t *)user_data;

    context->line_count++;

    if (length < sizeof(context->last_line))
    {
        memcpy(context->last_line, line, length);
        context->last_line[length] = '\0';
    }

    context->last_length = length;
}

/*
 * 一次传入多条完整消息 + 一条不完整的半条消息。
 */
static int test_multiple_lines(void)
{
    char buffer[256];
    size_t buffer_length;
    test_context_t context;

    memset(&context, 0, sizeof(context));

    /*
     * 两条完整消息 + 一条没有'\n'结尾的半条消息。
     */
    strcpy(buffer, "{\"seq\":1}\n{\"seq\":2}\n{\"seq\":3");
    buffer_length = strlen(buffer);

    line_parser_feed(
        buffer,
        &buffer_length,
        sizeof(buffer),
        on_line,
        &context
    );

    if (context.line_count != 2)
    {
        printf("[FAIL] expected 2 lines, got %d\n", context.line_count);
        return -1;
    }

    if (strcmp(context.last_line, "{\"seq\":2}") != 0)
    {
        printf("[FAIL] last line mismatch: %s\n", context.last_line);
        return -1;
    }

    /*
     * 半条消息必须保留在缓冲区中。
     */
    if (buffer_length != 8U) /* strlen("{\"seq\":3}") == 8 */
    {
        printf("[FAIL] expected 8 bytes remaining, got %zu\n", buffer_length);
        return -1;
    }

    if (strncmp(buffer, "{\"seq\":3}", buffer_length) != 0)
    {
        printf("[FAIL] remaining buffer mismatch: %s\n", buffer);
        return -1;
    }

    printf("[PASS] Multiple lines test\n");
    return 0;
}

/*
 * 处理\r\n行尾。
 */
static int test_crlf(void)
{
    char buffer[256];
    size_t buffer_length;
    test_context_t context;

    memset(&context, 0, sizeof(context));

    strcpy(buffer, "hello\r\nworld\r\n");
    buffer_length = strlen(buffer);

    line_parser_feed(
        buffer,
        &buffer_length,
        sizeof(buffer),
        on_line,
        &context
    );

    if (context.line_count != 2)
    {
        printf("[FAIL] expected 2 lines, got %d\n", context.line_count);
        return -1;
    }

    if (strcmp(context.last_line, "world") != 0)
    {
        printf("[FAIL] CRLF not stripped: %s\n", context.last_line);
        return -1;
    }

    if (buffer_length != 0U)
    {
        printf("[FAIL] buffer not empty after all lines: %zu\n", buffer_length);
        return -1;
    }

    printf("[PASS] CRLF handling test\n");
    return 0;
}

/*
 * 不完整数据（无'\n'）时不产生回调。
 */
static int test_no_newline(void)
{
    char buffer[256];
    size_t buffer_length;
    test_context_t context;

    memset(&context, 0, sizeof(context));

    strcpy(buffer, "no newline here");
    buffer_length = strlen(buffer);

    line_parser_feed(
        buffer,
        &buffer_length,
        sizeof(buffer),
        on_line,
        &context
    );

    if (context.line_count != 0)
    {
        printf("[FAIL] expected 0 lines, got %d\n", context.line_count);
        return -1;
    }

    printf("[PASS] No-newline test\n");
    return 0;
}

/*
 * 分两次喂入：第一次半条，第二次补全。
 */
static int test_split_across_feed(void)
{
    char buffer[256];
    size_t buffer_length;
    test_context_t context;

    memset(&context, 0, sizeof(context));

    strcpy(buffer, "{\"a\":1}");
    buffer_length = strlen(buffer);

    /* 第一次：无'\n'，不产生回调 */
    line_parser_feed(buffer, &buffer_length, sizeof(buffer), on_line, &context);

    if (context.line_count != 0)
    {
        printf("[FAIL] first feed should produce 0 lines\n");
        return -1;
    }

    /* 第二次：补上'\n'，产生回调 */
    strcpy(buffer + buffer_length, "\n");
    buffer_length += 1U;

    line_parser_feed(buffer, &buffer_length, sizeof(buffer), on_line, &context);

    if (context.line_count != 1)
    {
        printf("[FAIL] expected 1 line after second feed\n");
        return -1;
    }

    if (strcmp(context.last_line, "{\"a\":1}") != 0)
    {
        printf("[FAIL] line mismatch: %s\n", context.last_line);
        return -1;
    }

    printf("[PASS] Split-across-feed test\n");
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_multiple_lines();
    failures += test_crlf();
    failures += test_no_newline();
    failures += test_split_across_feed();

    if (failures != 0)
    {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }

    printf("\nAll line_parser tests passed\n");
    return 0;
}
