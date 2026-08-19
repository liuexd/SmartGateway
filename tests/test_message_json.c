#include "message_json.h"

#include <stdio.h>
#include <string.h>

static int test_build_data(void)
{
    frame_data_t data;
    char json[256];
    int length;

    memset(&data, 0, sizeof(data));

    strcpy(data.node_id, "NODE01");
    data.sequence = 1;

    snprintf(data.fields[0].key, sizeof(data.fields[0].key), "T");
    snprintf(data.fields[0].value, sizeof(data.fields[0].value), "%d", 253);
    snprintf(data.fields[1].key, sizeof(data.fields[1].key), "H");
    snprintf(data.fields[1].value, sizeof(data.fields[1].value), "%d", 601);
    data.field_count = 2;

    length = message_json_build_data(json, sizeof(json), &data);

    if (length < 0)
    {
        printf("[FAIL] build_data returned %d\n", length);
        return -1;
    }

    printf("Generated: %s", json);

    if (strcmp(
            json,
            "{\"node\":\"NODE01\",\"seq\":1,\"type\":\"DATA\","
            "\"fields\":{\"T\":\"253\",\"H\":\"601\"}}\n"
        ) != 0)
    {
        printf("[FAIL] DATA JSON mismatch\n");
        return -1;
    }

    if ((size_t)length != strlen(json))
    {
        printf("[FAIL] returned length mismatch\n");
        return -1;
    }

    printf("[PASS] build_data test\n");
    return 0;
}

static int test_build_ack(void)
{
    frame_ack_t ack;
    char json[256];
    int length;

    memset(&ack, 0, sizeof(ack));

    strcpy(ack.node_id, "NODE01");
    ack.sequence = 42;

    snprintf(ack.fields[0].key, sizeof(ack.fields[0].key), "LED");
    snprintf(ack.fields[0].value, sizeof(ack.fields[0].value), "%d", 1);
    ack.field_count = 1;

    length = message_json_build_ack(json, sizeof(json), &ack);

    if (length < 0)
    {
        printf("[FAIL] build_ack returned %d\n", length);
        return -1;
    }

    printf("Generated: %s", json);

    if (strcmp(
            json,
            "{\"node\":\"NODE01\",\"seq\":42,\"type\":\"ACK\","
            "\"fields\":{\"LED\":\"1\"}}\n"
        ) != 0)
    {
        printf("[FAIL] ACK JSON mismatch\n");
        return -1;
    }

    printf("[PASS] build_ack test\n");
    return 0;
}

static int test_build_nack(void)
{
    frame_nack_t nack;
    char json[256];
    int length;

    memset(&nack, 0, sizeof(nack));

    strcpy(nack.node_id, "NODE01");
    nack.sequence = 7;
    strcpy(nack.error, "bad_cmd");

    length = message_json_build_nack(json, sizeof(json), &nack);

    if (length < 0)
    {
        printf("[FAIL] build_nack returned %d\n", length);
        return -1;
    }

    printf("Generated: %s", json);

    if (strcmp(
            json,
            "{\"node\":\"NODE01\",\"seq\":7,\"type\":\"NACK\","
            "\"error\":\"bad_cmd\"}\n"
        ) != 0)
    {
        printf("[FAIL] NACK JSON mismatch\n");
        return -1;
    }

    printf("[PASS] build_nack test\n");
    return 0;
}

static int test_build_command(void)
{
    frame_command_t command;
    char json[256];
    int length;

    memset(&command, 0, sizeof(command));

    strcpy(command.sender, "GATEWAY");
    command.sequence = 1;

    /* 多参数命令：LED=1、MOTOR=50 */
    snprintf(command.fields[0].key, sizeof(command.fields[0].key), "LED");
    snprintf(command.fields[0].value, sizeof(command.fields[0].value), "%d", 1);
    snprintf(command.fields[1].key, sizeof(command.fields[1].key), "MOTOR");
    snprintf(command.fields[1].value, sizeof(command.fields[1].value), "%d", 50);
    command.field_count = 2;

    length = message_json_build_command(json, sizeof(json), &command);

    if (length < 0)
    {
        printf("[FAIL] build_command returned %d\n", length);
        return -1;
    }

    printf("Generated: %s", json);

    if (strcmp(
            json,
            "{\"node\":\"GATEWAY\",\"seq\":1,\"type\":\"CMD\","
            "\"fields\":{\"LED\":\"1\",\"MOTOR\":\"50\"}}\n"
        ) != 0)
    {
        printf("[FAIL] CMD JSON mismatch\n");
        return -1;
    }

    if ((size_t)length != strlen(json))
    {
        printf("[FAIL] returned length mismatch\n");
        return -1;
    }

    /* 生成的JSON应能被decode_command反向解析 */
    {
        frame_command_t decoded;
        int consumed;

        memset(&decoded, 0, sizeof(decoded));

        consumed = message_json_decode_command(
            json,
            (size_t)length,
            &decoded
        );

        if (consumed != length - 1 ||
            strcmp(decoded.sender, "GATEWAY") != 0 ||
            decoded.sequence != 1 ||
            strcmp(frame_fields_find(decoded.fields, decoded.field_count, "LED"), "1") != 0 ||
            strcmp(frame_fields_find(decoded.fields, decoded.field_count, "MOTOR"), "50") != 0)
        {
            printf("[FAIL] build_command round-trip mismatch\n");
            return -1;
        }
    }

    printf("[PASS] build_command test\n");
    return 0;
}

static int test_null_params(void)
{
    char json[256];

    if (message_json_build_data(json, sizeof(json), NULL) != -1)
    {
        printf("[FAIL] NULL data should return -1\n");
        return -1;
    }

    if (message_json_build_data(NULL, sizeof(json), NULL) != -1)
    {
        printf("[FAIL] NULL out should return -1\n");
        return -1;
    }

    printf("[PASS] NULL param test\n");
    return 0;
}

static int test_decode_command(void)
{
    frame_command_t cmd;
    const char *json;
    size_t json_length;
    int consumed;

    memset(&cmd, 0, sizeof(cmd));

    /* 新格式：fields 对象（多参数），带结尾换行 */
    json = "{\"node\":\"GATEWAY\",\"seq\":1,\"type\":\"CMD\","
           "\"fields\":{\"LED\":\"1\",\"MOTOR\":\"50\"}}\n";
    json_length = strlen(json);

    consumed = message_json_decode_command(json, json_length, &cmd);

    if (consumed != (int)(json_length - 1))
    {
        printf("[FAIL] decode_command consumed=%d, expected=%u\n",
               consumed, (unsigned int)(json_length - 1));
        return -1;
    }

    if (strcmp(cmd.sender, "GATEWAY") != 0 ||
        cmd.sequence != 1 ||
        strcmp(frame_fields_find(cmd.fields, cmd.field_count, "LED"), "1") != 0 ||
        strcmp(frame_fields_find(cmd.fields, cmd.field_count, "MOTOR"), "50") != 0)
    {
        printf("[FAIL] decode_command fields mismatch "
               "(sender=%s seq=%u fields=%zu)\n",
               cmd.sender,
               (unsigned int)cmd.sequence,
               cmd.field_count);
        return -1;
    }

    printf("[PASS] decode_command basic test\n");

    /* 旧格式兼容：数字形式的led自动转 LED 字段 */
    memset(&cmd, 0, sizeof(cmd));

    json = "{\"node\":\"GW01\",\"seq\":42,\"type\":\"CMD\",\"led\":1}\n";
    json_length = strlen(json);

    consumed = message_json_decode_command(json, json_length, &cmd);

    if (consumed < 0 ||
        strcmp(cmd.sender, "GW01") != 0 ||
        cmd.sequence != 42 ||
        strcmp(frame_fields_find(cmd.fields, cmd.field_count, "LED"), "1") != 0)
    {
        printf("[FAIL] decode_command legacy-led test failed\n");
        return -1;
    }

    printf("[PASS] decode_command legacy-led test\n");

    /* 旧格式兼容：字符串形式的led */
    memset(&cmd, 0, sizeof(cmd));

    json = "{\"node\":\"GW01\",\"seq\":42,\"type\":\"CMD\",\"led\":\"1\"}";
    json_length = strlen(json);

    consumed = message_json_decode_command(json, json_length, &cmd);

    if (consumed < 0 ||
        strcmp(cmd.sender, "GW01") != 0 ||
        cmd.sequence != 42 ||
        strcmp(frame_fields_find(cmd.fields, cmd.field_count, "LED"), "1") != 0)
    {
        printf("[FAIL] decode_command string-led test failed\n");
        return -1;
    }

    printf("[PASS] decode_command string-led test\n");

    /* 字段顺序无关 + 无关字段被忽略 */
    memset(&cmd, 0, sizeof(cmd));

    json = "{\"type\":\"CMD\",\"fields\":{\"LED\":\"0\"},\"node\":\"NODE01\","
           "\"seq\":7,\"temperature\":25.3}";
    json_length = strlen(json);

    consumed = message_json_decode_command(json, json_length, &cmd);

    if (consumed < 0 ||
        strcmp(cmd.sender, "NODE01") != 0 ||
        cmd.sequence != 7 ||
        strcmp(frame_fields_find(cmd.fields, cmd.field_count, "LED"), "0") != 0)
    {
        printf("[FAIL] decode_command unordered fields test failed\n");
        return -1;
    }

    printf("[PASS] decode_command unordered fields test\n");

    /* 错误输入：type不是CMD */
    memset(&cmd, 0, sizeof(cmd));

    json = "{\"node\":\"GATEWAY\",\"seq\":1,\"type\":\"DATA\","
           "\"fields\":{\"LED\":\"1\"}}";

    if (message_json_decode_command(json, strlen(json), &cmd) >= 0)
    {
        printf("[FAIL] decode_command should reject non-CMD type\n");
        return -1;
    }

    printf("[PASS] decode_command wrong-type test\n");

    /* 错误输入：缺少必需字段 */
    memset(&cmd, 0, sizeof(cmd));

    json = "{\"node\":\"GATEWAY\",\"seq\":1,\"fields\":{\"LED\":\"1\"}}";

    if (message_json_decode_command(json, strlen(json), &cmd) >= 0)
    {
        printf("[FAIL] decode_command should reject missing fields\n");
        return -1;
    }

    printf("[PASS] decode_command missing-field test\n");

    /* 错误输入：非法JSON */
    memset(&cmd, 0, sizeof(cmd));

    json = "{\"node\":\"GATEWAY\",\"seq\":1,}";

    if (message_json_decode_command(json, strlen(json), &cmd) >= 0)
    {
        printf("[FAIL] decode_command should reject malformed JSON\n");
        return -1;
    }

    printf("[PASS] decode_command malformed-json test\n");

    /* 错误输入：NULL参数 */
    if (message_json_decode_command(NULL, 0, &cmd) != -1)
    {
        printf("[FAIL] NULL line should return -1\n");
        return -1;
    }

    if (message_json_decode_command(json, strlen(json), NULL) != -1)
    {
        printf("[FAIL] NULL command should return -1\n");
        return -1;
    }

    printf("[PASS] decode_command NULL param test\n");

    return 0;
}

static int test_decode_data(void)
{
    frame_data_t data;
    const char *json;
    size_t json_length;
    int consumed;

    /* 新格式：fields 对象（字符串值） + 带结尾换行 */
    memset(&data, 0, sizeof(data));

    json = "{\"node\":\"NODE01\",\"seq\":1,\"type\":\"DATA\","
           "\"fields\":{\"T\":\"253\",\"H\":\"601\"}}\n";
    json_length = strlen(json);

    consumed = message_json_decode_data(json, json_length, &data);

    if (consumed != (int)(json_length - 1))
    {
        printf("[FAIL] decode_data consumed=%d, expected=%u\n",
               consumed, (unsigned int)(json_length - 1));
        return -1;
    }

    if (strcmp(data.node_id, "NODE01") != 0 ||
        data.sequence != 1 ||
        strcmp(frame_data_find_field(&data, "T"), "253") != 0 ||
        strcmp(frame_data_find_field(&data, "H"), "601") != 0)
    {
        printf("[FAIL] decode_data fields mismatch "
               "(node=%s seq=%u fields=%zu)\n",
               data.node_id,
               (unsigned int)data.sequence,
               data.field_count);
        return -1;
    }

    printf("[PASS] decode_data basic test\n");

    /* 字段顺序无关 + 无关字段被忽略 + 数字值字段 */
    memset(&data, 0, sizeof(data));

    json = "{\"fields\":{\"H\":500,\"T\":-50,\"P\":1013},\"type\":\"DATA\","
           "\"node\":\"NODE02\",\"seq\":7,\"led\":1}";

    consumed = message_json_decode_data(json, strlen(json), &data);

    if (consumed < 0 ||
        strcmp(data.node_id, "NODE02") != 0 ||
        data.sequence != 7 ||
        strcmp(frame_data_find_field(&data, "T"), "-50") != 0 ||
        strcmp(frame_data_find_field(&data, "H"), "500") != 0 ||
        strcmp(frame_data_find_field(&data, "P"), "1013") != 0 ||
        data.field_count != 3U)
    {
        printf("[FAIL] decode_data unordered/extra field test failed\n");
        return -1;
    }

    printf("[PASS] decode_data unordered fields test\n");

    /* 旧格式兼容：temperature/humidity 数字转 T/H 字段 */
    memset(&data, 0, sizeof(data));

    json = "{\"node\":\"NODE01\",\"seq\":1,\"type\":\"DATA\","
           "\"temperature\":25.3,\"humidity\":60.1}\n";

    consumed = message_json_decode_data(json, strlen(json), &data);

    if (consumed < 0 ||
        strcmp(frame_data_find_field(&data, "T"), "253") != 0 ||
        strcmp(frame_data_find_field(&data, "H"), "601") != 0)
    {
        printf("[FAIL] decode_data legacy format test failed\n");
        return -1;
    }

    printf("[PASS] decode_data legacy format test\n");

    /* 错误输入：type不是DATA */
    memset(&data, 0, sizeof(data));

    json = "{\"node\":\"NODE01\",\"seq\":1,\"type\":\"CMD\","
           "\"fields\":{\"T\":\"253\"}}";

    if (message_json_decode_data(json, strlen(json), &data) >= 0)
    {
        printf("[FAIL] decode_data should reject non-DATA type\n");
        return -1;
    }

    printf("[PASS] decode_data wrong-type test\n");

    /* 错误输入：缺失必需字段（缺 seq） */
    memset(&data, 0, sizeof(data));

    json = "{\"node\":\"NODE01\",\"type\":\"DATA\","
           "\"fields\":{\"T\":\"253\"}}";

    if (message_json_decode_data(json, strlen(json), &data) >= 0)
    {
        printf("[FAIL] decode_data should reject missing seq\n");
        return -1;
    }

    printf("[PASS] decode_data missing-field test\n");

    /* 错误输入：fields 不是对象 */
    memset(&data, 0, sizeof(data));

    json = "{\"node\":\"NODE01\",\"seq\":1,\"type\":\"DATA\","
           "\"fields\":\"T=253\"}";

    if (message_json_decode_data(json, strlen(json), &data) >= 0)
    {
        printf("[FAIL] decode_data should reject non-object fields\n");
        return -1;
    }

    printf("[PASS] decode_data bad-fields test\n");

    /* 错误输入：NULL参数 */
    memset(&data, 0, sizeof(data));

    if (message_json_decode_data(NULL, 0, &data) != -1)
    {
        printf("[FAIL] NULL line should return -1\n");
        return -1;
    }

    json = "{\"node\":\"NODE01\",\"seq\":1,\"type\":\"DATA\","
           "\"fields\":{\"T\":\"253\"}}";

    if (message_json_decode_data(json, strlen(json), NULL) != -1)
    {
        printf("[FAIL] NULL data should return -1\n");
        return -1;
    }

    printf("[PASS] decode_data NULL param test\n");

    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_build_data();
    failures += test_build_ack();
    failures += test_build_nack();
    failures += test_build_command();
    failures += test_null_params();
    failures += test_decode_command();
    failures += test_decode_data();

    if (failures != 0)
    {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }

    printf("\nAll message_json tests passed\n");
    return 0;
}
