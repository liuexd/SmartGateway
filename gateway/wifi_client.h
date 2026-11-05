/* 模块 wifi_client：每个 WiFi worker 独占客户端 socket；group 跟踪 detached 线程。 */
#ifndef WIFI_CLIENT_H
#define WIFI_CLIENT_H

#include <pthread.h>
#include <stddef.h>

#include "frame_parser.h"
#include "message_queue.h"
#include "device_manager.h"

/*
 * 活跃 worker 线程计数与退出同步。
 *
 * mutex         ：保护 active_workers
 * all_stopped   ：active_workers 归零时广播，唤醒等待者
 * active_workers：当前在跑的 worker 线程数
 */
typedef struct
{
    pthread_mutex_t mutex;
    pthread_cond_t all_stopped;

    size_t active_workers;
} wifi_worker_group_t;

/*
 * 每个 WiFi client thread 独有的启动参数。
 */
typedef struct
{
    int client_fd;

    /* 运行状态与统计；上行消息通过 upstream_queue 转交。 */
    gateway_context_t *gateway_context;

    /* 主循环设置的共享运行标志。 */
    const volatile int *running;

    device_manager_t *device_manager;

    message_queue_t *upstream_queue;

    /* detached Worker 退出时递减计数，通知等待者。 */
    wifi_worker_group_t *wifi_worker_group;

} wifi_client_context_t;

/*
 * pthread 入口：接管 arg 指向的上下文和 client_fd，退出时关闭并释放。
 */

void *wifi_client_worker(void *arg);

/* 主循环初始化线程组；成功返回 0，失败返回 -1。 */
int wifi_worker_group_init(wifi_worker_group_t *group);

/* 创建 Worker 前递增；创建失败时由主循环调用 remove 回滚。 */
void wifi_worker_group_add(wifi_worker_group_t *group);

/* Worker 退出时递减，归零时唤醒等待线程。 */
void wifi_worker_group_remove(wifi_worker_group_t *group);

/* 等待所有 detached Worker 退出；调用期间可能阻塞。 */
void wifi_worker_group_wait(wifi_worker_group_t *group);

/* 所有 Worker 退出后销毁同步对象。 */
void wifi_worker_group_destroy(wifi_worker_group_t *group);

#endif