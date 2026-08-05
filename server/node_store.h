#ifndef NODE_STORE_H
#define NODE_STORE_H

#include "frame_parser.h"

#include <stddef.h>

/*
 * =====================================================================
 * 节点数据存储（node_store）
 *
 * 职责：
 *   维护每个节点的最新温湿度数据，支持按 node_id 查询/更新。
 *
 * 实现说明（设计见 node_store.c）：
 *   - 采用"动态数组 + 线性查找"：节点数通常很少（<100），
 *     O(n) 查找足够快，且内存紧凑、实现简单；
 *   - 存储满时按 last_update 淘汰最久未更新的节点（LRU），
 *     保证新节点总能上报数据；
 *   - 当前服务器为单线程模型，未加锁；
 *     若将来多线程访问，需由调用者保证互斥。
 * =====================================================================
 */

/*
 * 节点存储句柄（不透明类型）。
 */
typedef struct node_store node_store_t;

/*
 * 创建节点存储。
 *
 * @param max_nodes 最多容纳的节点数（必须 > 0）
 *
 * @return 成功返回句柄，失败返回NULL
 */
node_store_t *node_store_create(size_t max_nodes);

/*
 * 销毁节点存储，释放全部资源。
 */
void node_store_destroy(node_store_t *store);

/*
 * 更新（或新增）一个节点的最新数据。
 *
 * 行为：
 *   - node_id 已存在：覆盖其数据，并刷新 last_update；
 *   - node_id 不存在且未满：新增槽位；
 *   - node_id 不存在但已满：淘汰最久未更新的节点后写入。
 *
 * @return 0成功，-1失败（参数错误）
 */
int node_store_update(
    node_store_t *store,
    const frame_data_t *data
);

/*
 * 按节点编号查询最新数据。
 *
 * @param out 输出查询结果
 *
 * @return 0找到，-1未找到或参数错误
 */
int node_store_query(
    const node_store_t *store,
    const char *node_id,
    frame_data_t *out
);

#endif
