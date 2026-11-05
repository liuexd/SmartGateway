/* 模块 server_link：维护北向 TCP 连接、发送上行队列并接收 Server 命令。 */
#ifndef SERVER_LINK_H
#define SERVER_LINK_H

#include "gateway_app.h"
#include "message_queue.h"

/*
 * Server Link Thread 启动参数。
 *
 * Server Link Worker 是运行期间北向 TCP 连接的唯一拥有者。
 * 调用方必须让上下文及共享资源保持有效，直到线程退出并 join。
 */
typedef struct
{
    gateway_context_t *gateway_context;

    /*
     * Bluetooth/WiFi产生的上行JSON
     * 都进入这个队列。
     */
    message_queue_t *upstream_queue;

    device_manager_t *device_manager;

    /*
     * Gateway全局运行标志。
     */
    const volatile int *running;

} server_link_context_t;


/*
 * Server Link pthread入口。
 */
void *server_link_worker(
    void *arg
);

#endif