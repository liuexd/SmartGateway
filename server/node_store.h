#ifndef NODE_STORE_H
#define NODE_STORE_H

#include "frame_parser.h"

#include <stddef.h>

/*
 * =====================================================================
 * 节点数据存储（node_store）
 *
 * 职责：
 *   维护每个节点的最新数据（通用键值对），支持按 node_id 查询/更新。
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

/*
 * 查询节点的设备类型。
 *
 * 从该节点最近一次上报的 DEV 字段解析（见 frame_device_type_from_text），
 * 节点未上报 DEV 时返回 FRAME_DEVICE_UNKNOWN。
 *
 * @param store    节点存储
 * @param node_id  节点编号
 * @param type_out 输出：设备类型
 *
 * @return 0找到节点；-1节点不存在或参数错误
 */
int node_store_query_device_type(
    const node_store_t *store,
    const char *node_id,
    frame_device_type_t *type_out
);

/*
 * 判断节点是否在线。
 *
 * 判定规则：最近一次上报时间距今 < timeout_sec 视为在线，
 * 否则视为离线（超时未上报）。
 *
 * @param store       节点存储
 * @param node_id     节点编号
 * @param timeout_sec 在线判定阈值（秒）
 * @param online_out  输出：1在线，0离线
 *
 * @return 0找到节点；-1节点不存在或参数错误
 */
int node_store_is_online(
    const node_store_t *store,
    const char *node_id,
    time_t timeout_sec,
    int *online_out
);

/*
 * 节点遍历回调。
 *
 * @param data        节点最新数据
 * @param last_update 最近一次上报时间（time(NULL)）
 * @param user_data   透传的用户数据
 *
 * @return 0继续遍历，非0停止遍历
 */
typedef int (*node_store_visit_cb_t)(
    const frame_data_t *data,
    time_t last_update,
    void *user_data
);

/*
 * 遍历所有已注册节点。
 *
 * 按注册顺序依次回调；回调返回非0时提前停止。
 *
 * @param store    节点存储
 * @param callback 遍历回调（不可为NULL）
 * @param user_data 透传给回调的用户数据
 */
void node_store_foreach(
    const node_store_t *store,
    node_store_visit_cb_t callback,
    void *user_data
);

#endif
