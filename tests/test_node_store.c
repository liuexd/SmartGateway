#include "node_store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 构造一个带 T/H 字段的数据。
 */
static frame_data_t make_data(
    const char *node_id,
    uint32_t sequence,
    int temperature_x10,
    int humidity_x10
)
{
    frame_data_t data;

    memset(&data, 0, sizeof(data));
    strncpy(data.node_id, node_id, sizeof(data.node_id) - 1U);
    data.sequence = sequence;

    snprintf(data.fields[0].key, sizeof(data.fields[0].key), "T");
    snprintf(data.fields[0].value, sizeof(data.fields[0].value), "%d", temperature_x10);
    snprintf(data.fields[1].key, sizeof(data.fields[1].key), "H");
    snprintf(data.fields[1].value, sizeof(data.fields[1].value), "%d", humidity_x10);
    data.field_count = 2;

    return data;
}

/*
 * 构造一个带 DEV 设备类型字段的数据。
 */
static frame_data_t make_data_dev(
    const char *node_id,
    uint32_t sequence,
    const char *dev_text)
{
    frame_data_t data;

    memset(&data, 0, sizeof(data));
    strncpy(data.node_id, node_id, sizeof(data.node_id) - 1U);
    data.sequence = sequence;

    snprintf(data.fields[0].key, sizeof(data.fields[0].key), FRAME_KV_DEV);
    snprintf(data.fields[0].value, sizeof(data.fields[0].value), "%s", dev_text);
    data.field_count = 1;

    return data;
}

static int test_create_destroy(void)
{
    node_store_t *store;

    if (node_store_create(0) != NULL)
    {
        printf("[FAIL] create(0) should return NULL\n");
        return -1;
    }

    store = node_store_create(4);

    if (store == NULL)
    {
        printf("[FAIL] create(4) failed\n");
        return -1;
    }

    node_store_destroy(store);
    node_store_destroy(NULL);   /* 应安全 */

    printf("[PASS] create/destroy test\n");
    return 0;
}

static int test_update_query(void)
{
    node_store_t *store;
    frame_data_t data;
    frame_data_t out;

    store = node_store_create(4);

    if (store == NULL)
    {
        return -1;
    }

    /* 新增节点 */
    data = make_data("NODE01", 1, 253, 601);

    if (node_store_update(store, &data) != 0)
    {
        printf("[FAIL] first update failed\n");
        node_store_destroy(store);
        return -1;
    }

    /* 查询到 */
    if (node_store_query(store, "NODE01", &out) != 0 ||
        strcmp(out.node_id, "NODE01") != 0 ||
        out.sequence != 1 ||
        frame_data_find_field(&out, "T") == NULL ||
        strcmp(frame_data_find_field(&out, "T"), "253") != 0 ||
        strcmp(frame_data_find_field(&out, "H"), "601") != 0)
    {
        printf("[FAIL] query after insert mismatch\n");
        node_store_destroy(store);
        return -1;
    }

    /* 覆盖更新 */
    data = make_data("NODE01", 2, 260, 580);

    if (node_store_update(store, &data) != 0)
    {
        printf("[FAIL] second update failed\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_query(store, "NODE01", &out) != 0 ||
        out.sequence != 2 ||
        strcmp(frame_data_find_field(&out, "T"), "260") != 0)
    {
        printf("[FAIL] query after overwrite mismatch\n");
        node_store_destroy(store);
        return -1;
    }

    /* 未找到 */
    if (node_store_query(store, "NODE99", &out) != -1)
    {
        printf("[FAIL] query unknown node should return -1\n");
        node_store_destroy(store);
        return -1;
    }

    node_store_destroy(store);

    printf("[PASS] update/query test\n");
    return 0;
}

static int test_lru_eviction(void)
{
    node_store_t *store;
    frame_data_t data;
    frame_data_t out;

    store = node_store_create(2);

    if (store == NULL)
    {
        return -1;
    }

    /* 塞满2个槽位 */
    data = make_data("NODE01", 1, 100, 100);
    node_store_update(store, &data);
    data = make_data("NODE02", 2, 200, 200);
    node_store_update(store, &data);

    /*
     * last_update 使用 time()（秒级精度），
     * 必须等待1秒让时间戳可区分。
     */
    sleep(1);

    /* 再次更新NODE01，使其成为最新 */
    data = make_data("NODE01", 3, 300, 300);
    node_store_update(store, &data);

    /*
     * 此时 NODE02 是最旧的（从未刷新），
     * 新节点 NODE03 到来应淘汰 NODE02。
     */
    sleep(1);

    data = make_data("NODE03", 4, 400, 400);
    node_store_update(store, &data);

    if (node_store_query(store, "NODE02", &out) != -1)
    {
        printf("[FAIL] NODE02 should have been evicted\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_query(store, "NODE01", &out) != 0 ||
        node_store_query(store, "NODE03", &out) != 0)
    {
        printf("[FAIL] NODE01/NODE03 should be present\n");
        node_store_destroy(store);
        return -1;
    }

    node_store_destroy(store);

    printf("[PASS] LRU eviction test\n");
    return 0;
}

static int test_null_params(void)
{
    node_store_t *store;
    frame_data_t data;
    frame_data_t out;
    frame_device_type_t dev_type;
    int online;

    store = node_store_create(2);

    if (store == NULL)
    {
        return -1;
    }

    if (node_store_update(NULL, &data) != -1)
    {
        printf("[FAIL] update NULL store should return -1\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_update(store, NULL) != -1)
    {
        printf("[FAIL] update NULL data should return -1\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_query(store, NULL, &out) != -1)
    {
        printf("[FAIL] query NULL node_id should return -1\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_query(store, "NODE01", NULL) != -1)
    {
        printf("[FAIL] query NULL out should return -1\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_query_device_type(store, "NODE99", &dev_type) != -1)
    {
        printf("[FAIL] query_device_type unknown node should return -1\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_is_online(store, "NODE99", 30, &online) != -1)
    {
        printf("[FAIL] is_online unknown node should return -1\n");
        node_store_destroy(store);
        return -1;
    }

    node_store_destroy(store);

    printf("[PASS] NULL param test\n");
    return 0;
}

/*
 * 设备类型/在线状态/遍历测试。
 */
static int test_device_type_and_online(void)
{
    node_store_t *store;
    frame_data_t data;
    frame_device_type_t dev_type;
    int online;

    store = node_store_create(4);

    if (store == NULL)
    {
        return -1;
    }

    /* 温湿度传感器：DEV=1 */
    data = make_data_dev("NODE01", 1, "1");
    node_store_update(store, &data);

    /* 继电器：DEV=2 */
    data = make_data_dev("NODE02", 1, "2");
    node_store_update(store, &data);

    /* 未上报DEV的节点 */
    data = make_data("NODE03", 1, 253, 601);
    node_store_update(store, &data);

    if (node_store_query_device_type(store, "NODE01", &dev_type) != 0 ||
        dev_type != FRAME_DEVICE_THSENSOR)
    {
        printf("[FAIL] NODE01 device type should be THSENSOR\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_query_device_type(store, "NODE02", &dev_type) != 0 ||
        dev_type != FRAME_DEVICE_RELAY)
    {
        printf("[FAIL] NODE02 device type should be RELAY\n");
        node_store_destroy(store);
        return -1;
    }

    if (node_store_query_device_type(store, "NODE03", &dev_type) != 0 ||
        dev_type != FRAME_DEVICE_UNKNOWN)
    {
        printf("[FAIL] NODE03 device type should be UNKNOWN\n");
        node_store_destroy(store);
        return -1;
    }

    /* 刚上报的节点应为在线（超时阈值30秒） */
    if (node_store_is_online(store, "NODE01", 30, &online) != 0 ||
        online != 1)
    {
        printf("[FAIL] NODE01 should be online\n");
        node_store_destroy(store);
        return -1;
    }

    /* 超时阈值为0：任何节点都视为离线 */
    if (node_store_is_online(store, "NODE01", 0, &online) != 0 ||
        online != 0)
    {
        printf("[FAIL] NODE01 should be offline with timeout 0\n");
        node_store_destroy(store);
        return -1;
    }

    node_store_destroy(store);

    printf("[PASS] device type / online test\n");
    return 0;
}

/*
 * 遍历测试。
 */
typedef struct
{
    size_t count;
    int found_node01;
    int found_node02;
} visit_ctx_t;

static int visit_cb(
    const frame_data_t *data,
    time_t last_update,
    void *user_data)
{
    visit_ctx_t *ctx = (visit_ctx_t *)user_data;

    (void)last_update;

    ctx->count++;

    if (strcmp(data->node_id, "NODE01") == 0)
    {
        ctx->found_node01 = 1;
    }

    if (strcmp(data->node_id, "NODE02") == 0)
    {
        ctx->found_node02 = 1;
    }

    return 0;
}

static int test_foreach(void)
{
    node_store_t *store;
    frame_data_t data;
    visit_ctx_t ctx;

    store = node_store_create(4);

    if (store == NULL)
    {
        return -1;
    }

    data = make_data("NODE01", 1, 253, 601);
    node_store_update(store, &data);
    data = make_data("NODE02", 2, 260, 580);
    node_store_update(store, &data);

    memset(&ctx, 0, sizeof(ctx));
    node_store_foreach(store, visit_cb, &ctx);

    if (ctx.count != 2U ||
        !ctx.found_node01 ||
        !ctx.found_node02)
    {
        printf("[FAIL] foreach did not visit all nodes\n");
        node_store_destroy(store);
        return -1;
    }

    /* NULL 回调应安全 */
    node_store_foreach(store, NULL, &ctx);
    node_store_foreach(NULL, visit_cb, &ctx);

    node_store_destroy(store);

    printf("[PASS] foreach test\n");
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_create_destroy();
    failures += test_update_query();
    failures += test_lru_eviction();
    failures += test_null_params();
    failures += test_device_type_and_online();
    failures += test_foreach();

    if (failures != 0)
    {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }

    printf("\nAll node_store tests passed\n");
    return 0;
}
