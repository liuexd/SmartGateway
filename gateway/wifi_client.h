#ifndef WIFI_CLIENT_H
#define WIFI_CLIENT_H

#include <pthread.h>
#include <stddef.h>

#include "frame_parser.h"
#include "message_queue.h"

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

    /*
     * 暂时仍复用现有 gateway_context。
     * M7-6 再改成 message queue。
     */
    gateway_context_t *gateway_context;

    /*
     *Gateway全局运行标志
     *按下Ctrl+C后变为0
     */
    const volatile int *running;
    message_queue_t *upstream_queue;

    /*
     *本worker所属的线程组，用于退出时计数-1。
     */
    wifi_worker_group_t *wifi_worker_group;

} wifi_client_context_t;

/*
 * pthread worker entry。
 */

void *wifi_client_worker(void *arg);

int wifi_worker_group_init(wifi_worker_group_t * group);

void wifi_worker_group_add(wifi_worker_group_t * group);

void wifi_worker_group_remove(wifi_worker_group_t * group);

void wifi_worker_group_wait(wifi_worker_group_t * group);

void wifi_worker_group_destroy(wifi_worker_group_t * group);


#endif