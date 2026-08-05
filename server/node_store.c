/*
 * =====================================================================
 * 节点数据存储（node_store）
 *
 * 数据结构：动态数组 + 线性查找。
 *
 * 选型理由：
 *   - 网关下辖节点数通常很少（几个到几十个），线性查找 O(n) 足够；
 *   - 动态数组内存紧凑、缓存友好，实现简单；
 *   - 避免哈希表的实现复杂度和额外内存开销。
 *
 * 淘汰策略：
 *   存储满且新节点到来时，淘汰 last_update 最旧的槽位（LRU），
 *   保证新节点总能上报数据。
 * =====================================================================
 */

#include "node_store.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * 单个槽位。
 *
 * used        ：1=占用，0=空闲
 * last_update ：最近一次更新时间（用于满时淘汰最旧节点）
 * data        ：节点数据（含 node_id）
 */
typedef struct node_entry
{
    int used;
    time_t last_update;
    frame_data_t data;
} node_entry_t;

/*
 * 存储主体。
 *
 * entries  ：槽位数组，按 max_nodes 预分配
 * capacity ：容量上限
 * count    ：当前占用槽位数（=已注册节点数）
 */
struct node_store
{
    node_entry_t *entries;
    size_t capacity;
    size_t count;
};

/*
 * 获取当前时间戳；time() 失败时返回 0，避免污染 last_update 语义。
 */
static time_t node_store_now(void)
{
    time_t now;

    now = time(NULL);

    if (now == (time_t)-1)
    {
        now = 0;
    }

    return now;
}

node_store_t *node_store_create(size_t max_nodes)
{
    node_store_t *store;

    if (max_nodes == 0U)
    {
        return NULL;
    }

    store = (node_store_t *)malloc(sizeof(*store));

    if (store == NULL)
    {
        return NULL;
    }

    store->entries = (node_entry_t *)calloc(
        max_nodes,
        sizeof(node_entry_t)
    );

    if (store->entries == NULL)
    {
        free(store);
        return NULL;
    }

    store->capacity = max_nodes;
    store->count = 0;

    return store;
}

void node_store_destroy(node_store_t *store)
{
    if (store == NULL)
    {
        return;
    }

    free(store->entries);
    free(store);
}

/*
 * 按 node_id 查找已占用的槽位。
 *
 * @return 找到返回槽位指针，未找到返回NULL
 */
static node_entry_t *node_store_find(
    node_store_t *store,
    const char *node_id)
{
    size_t i;

    for (i = 0; i < store->count; i++)
    {
        if (store->entries[i].used &&
            strcmp(store->entries[i].data.node_id, node_id) == 0)
        {
            return &store->entries[i];
        }
    }

    return NULL;
}

int node_store_update(
    node_store_t *store,
    const frame_data_t *data)
{
    node_entry_t *entry;
    time_t now;

    if (store == NULL ||
        data == NULL ||
        data->node_id[0] == '\0')
    {
        return -1;
    }

    now = node_store_now();

    entry = node_store_find(store, data->node_id);

    if (entry != NULL)
    {
        /* 已存在：覆盖数据，刷新时间戳 */
        entry->data = *data;
        entry->last_update = now;
        return 0;
    }

    if (store->count < store->capacity)
    {
        /* 未满：取下一个空闲槽位 */
        entry = &store->entries[store->count];
        store->count++;
    }
    else
    {
        /* 已满：淘汰最久未更新的槽位（LRU） */
        size_t oldest = 0;
        size_t i;

        for (i = 1; i < store->count; i++)
        {
            if (store->entries[i].last_update <
                store->entries[oldest].last_update)
            {
                oldest = i;
            }
        }

        entry = &store->entries[oldest];
    }

    memset(entry, 0, sizeof(*entry));
    entry->used = 1;
    entry->data = *data;
    entry->last_update = now;

    return 0;
}

int node_store_query(
    const node_store_t *store,
    const char *node_id,
    frame_data_t *out)
{
    size_t i;

    if (store == NULL ||
        node_id == NULL ||
        node_id[0] == '\0' ||
        out == NULL)
    {
        return -1;
    }

    for (i = 0; i < store->count; i++)
    {
        if (store->entries[i].used &&
            strcmp(store->entries[i].data.node_id, node_id) == 0)
        {
            *out = store->entries[i].data;
            return 0;
        }
    }

    return -1;
}
