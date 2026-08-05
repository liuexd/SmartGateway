#include "node_store.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * 构造一个温湿度数据。
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
    data.temperature_x10 = temperature_x10;
    data.humidity_x10 = humidity_x10;

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
        out.temperature_x10 != 253 ||
        out.humidity_x10 != 601)
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
        out.temperature_x10 != 260)
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

    node_store_destroy(store);

    printf("[PASS] NULL param test\n");
    return 0;
}

int main(void)
{
    int failures = 0;

    failures += test_create_destroy();
    failures += test_update_query();
    failures += test_lru_eviction();
    failures += test_null_params();

    if (failures != 0)
    {
        printf("\n%d test(s) failed\n", failures);
        return 1;
    }

    printf("\nAll node_store tests passed\n");
    return 0;
}
