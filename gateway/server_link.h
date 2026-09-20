#ifndef SERVER_LINK_H
#define SERVER_LINK_H

#include "gateway_app.h"
#include "message_queue.h"

/*
 * Server Link Thread 启动参数。
 *
 * Server Link Thread 后续将成为
 * Gateway <-> Server TCP连接的唯一拥有者。
 */
typedef struct
{
    gateway_context_t *gateway_context;

    /*
     * Bluetooth/WiFi产生的上行JSON
     * 都进入这个队列。
     */
    message_queue_t *upstream_queue;

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