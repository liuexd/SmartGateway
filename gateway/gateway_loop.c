/* 模块 gateway_loop：初始化共享资源、启动工作线程、接收 WiFi 连接并按依赖顺序回收。 */
#define _POSIX_C_SOURCE 200809L

#include "gateway_loop.h"
#include "gateway_app.h"
#include "wifi_server.h"
#include "wifi_client.h"
#include "bluetooth_worker.h"
#include "message_queue.h"
#include "server_link.h"
#include "device_manager.h"

#include  <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>

/*
 * WiFi 节点监听端口。
 *
 * 主循环只负责 accept 新的 WiFi 节点并派发 worker，
 * 因此只需监听这一个 fd：
 *   - 串口 I/O      -> bluetooth_worker 线程
 *   - 北向 TCP      -> server_link_worker 线程
 *   - WiFi 客户端   -> 各自 detached 的 wifi_client_worker 线程
 */
#define GATEWAY_WIFI_PORT 7000

int gateway_loop_run(
    const char *serial_device,
    int baud_rate,
    gateway_context_t *context,
    frame_parser_t *parser,
    const volatile int *running)
{

    // wifi_node-gateway通信专用
    //后续由于程序改为多线程，导致wifi_buffer、wifi_length、client_fd只属于每个work自己
    int wifi_listen_fd;
    wifi_worker_group_t wifi_worker_group;

    device_manager_t device_manager;

    message_queue_t upstream_queue;

    //北向连接服务器管理线程
    pthread_t server_thread;
    server_link_context_t server_worker_context;
    int server_thread_created = 0;

    //蓝牙节点
    pthread_t bluetooth_thread;
    bluetooth_worker_context_t bluetooth_worker_context;
    int bluetooth_thread_created = 0;

    if (context == NULL ||
        parser == NULL ||
        running == NULL)
    {
        return -1;
    }

    printf("Gateway started\n");

    if (serial_device != NULL)
    {
        printf("Serial device: %s\n", serial_device);
        printf("Baud rate    : %d, 8N1, raw mode\n", baud_rate);
    }
    else
    {
        printf("[BT] disabled (--no-serial)\n");
    }

    /* 初始化监听和共享资源；串口重连由 Bluetooth Worker 负责。 */

    wifi_listen_fd = wifi_server_open(GATEWAY_WIFI_PORT);

    if (wifi_listen_fd < 0)
    {
        fprintf(stderr, "[WIFI] cannot listen on port %d: %s\n", GATEWAY_WIFI_PORT, strerror(errno));
        return -1;
    }

    printf("[WIFI] listening on port %d\n", GATEWAY_WIFI_PORT);

    if(wifi_worker_group_init(&wifi_worker_group)!=0)
    {
        fprintf(stderr,
        "[WIFI]  worker group init failed\n"
        );

        wifi_server_close(wifi_listen_fd);

        return -1;
    }

    if(message_queue_init(&upstream_queue) != MESSAGE_QUEUE_OK)
    {
        fprintf(stderr,
            "[QUEUE] upstream queue init failed\n");

            wifi_worker_group_destroy(&wifi_worker_group);

            wifi_server_close(wifi_listen_fd);

            return -1;
    }

    if(device_manager_init(&device_manager) != DEVICE_MANAGER_OK)
    {
        fprintf(stderr,"[DEVICE] manager init failed\n");

        message_queue_destroy(&upstream_queue);

        wifi_worker_group_destroy(&wifi_worker_group);

        wifi_server_close(wifi_listen_fd);

        return -1;
    }

    /* Server Link Worker 独占北向 TCP 连接并消费上行队列。 */
    server_worker_context.gateway_context = context;
    server_worker_context.upstream_queue = &upstream_queue;
    server_worker_context.running = running;
    server_worker_context.device_manager = &device_manager;

    {
        int  pthread_result;

        pthread_result = pthread_create(&server_thread,NULL,
            server_link_worker,&server_worker_context);
        if(pthread_result != 0)
        {
            fprintf(stderr,
                "[SERVER LINK] pthread_create failed: %s\n",
                strerror(pthread_result));

                message_queue_destroy(&upstream_queue);

                wifi_worker_group_destroy(&wifi_worker_group);

                device_manager_destroy(&device_manager);

                wifi_server_close(wifi_listen_fd);

                return -1;
        }

        server_thread_created = 1;
    }

    /* Bluetooth Worker 独占串口，负责接收、命令下发和重连。 */
    if (serial_device != NULL)
    {
        bluetooth_worker_context.serial_device = serial_device;
        bluetooth_worker_context.baud_rate = baud_rate;
        bluetooth_worker_context.gateway_context = context;
        bluetooth_worker_context.parser = parser;
        bluetooth_worker_context.running = running;
        bluetooth_worker_context.upstream_queue = &upstream_queue;
        bluetooth_worker_context.device_manager = &device_manager;
        {
            int pthread_result;
            pthread_result = pthread_create(
                &bluetooth_thread,
                NULL,
                bluetooth_worker,
                &bluetooth_worker_context
            );

            if(pthread_result != 0)
            {
                fprintf(
                    stderr,
                    "[BT] pthread_create failed: %s\n",
                    strerror(pthread_result)
                );
                message_queue_shutdown(&upstream_queue);

                if(server_thread_created)
                {
                    pthread_join(server_thread,NULL);
                }
                message_queue_destroy(&upstream_queue);

                wifi_worker_group_destroy(&wifi_worker_group);

                wifi_server_close(wifi_listen_fd);

                return -1;
            }

            bluetooth_thread_created = 1;
        }
    }
    while (*running)
    {
        /*
            * 主循环只监听 wifi_listen_fd 一个 fd。
            *
            * 其余 I/O 都已在各自的 worker 线程中处理：
            *   - 串口            -> bluetooth_worker
            *   - 北向 TCP        -> server_link_worker
            *   - WiFi 客户端数据 -> wifi_client_worker
            */
        struct pollfd listen_poll_fd;
        int poll_result;

        listen_poll_fd.fd = wifi_listen_fd;
        listen_poll_fd.events = POLLIN;
        listen_poll_fd.revents = 0;


        poll_result = poll(&listen_poll_fd, 1, 1000);

        if (poll_result < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            fprintf(stderr, "[IO] poll failed: %s\n", strerror(errno));
            continue;
        }

        if (poll_result == 0)
        {
            /*
                * 一秒内没有数据不是错误。
                * 继续检查退出标志。
                */
            continue;
        }

        if ((listen_poll_fd.revents & POLLIN) != 0)
        {
            int accepted_client_fd;

            accepted_client_fd = wifi_server_accept(wifi_listen_fd);
            if (accepted_client_fd < 0)
            {
                if (errno != EINTR)
                {
                    fprintf(stderr,
                            "[WIFI] accept failed: %s\n",
                            strerror(errno));
                }
            }
            else
            {
                wifi_client_context_t *client;
                pthread_t client_thread;
                int pthread_result;
                //
                client = malloc(sizeof(*client));

                if(client == NULL)
                {
                    fprintf(stderr,
                        "[WIFI] malloc client context failed\n");
                        wifi_server_close(accepted_client_fd);

                }
                else{
                    client->client_fd = accepted_client_fd;
                    client->gateway_context = context;
                    client->running = running;
                    client->wifi_worker_group = &wifi_worker_group;
                    client->upstream_queue = &upstream_queue;
                    client->device_manager = &device_manager;

                    /* 创建前计数；创建失败回滚，成功后由 worker 退出时减计数。 */
                    wifi_worker_group_add(&wifi_worker_group);

                    pthread_result = pthread_create(
                        &client_thread,
                        NULL,
                        wifi_client_worker,
                        client
                    );

                    if(pthread_result != 0)
                    {
                        fprintf(
                            stderr,
                            "[WIFI] pthread_create failed: %s\n",
                            strerror(pthread_result)
                        );

                        wifi_server_close(accepted_client_fd);
                        free(client);
                        wifi_worker_group_remove(&wifi_worker_group);
                    }
                    else{
                        printf("[WIFI] worker create, fd=%d\n",accepted_client_fd);

                        /* detached worker 不能逐个 join；退出由 group 计数通知。 */
                        pthread_result = pthread_detach(client_thread);

                        if(pthread_result != 0)
                        {
                            fprintf(
                                stderr,
                                "[WIFI] pthread_detach failed: %s\n",
                                strerror(pthread_result)
                            );
                        }
                    }
                }
            }
        }

    }

    /*
     * 退出主循环后清理WiFi资源。不再接收新的wifi节点进行连接
     */
    wifi_server_close(wifi_listen_fd);

    printf("[BT] waitting for Bluetooth worker...\n");

    /*
     * 在等待生产者之前关闭队列，唤醒阻塞在 push 的线程。
     * 队列本身仍须保留到所有生产者和消费者退出。
     */
    message_queue_shutdown(&upstream_queue);

    if(bluetooth_thread_created)
    {
        pthread_join(bluetooth_thread,NULL);
    }
    printf("[BT] Bluetooth worker stopped\n");

    printf("[WIFI] waiting for client workers...\n");

    /* detached WiFi worker 通过计数和条件变量等待；其他线程用 join。 */
    wifi_worker_group_wait(&wifi_worker_group);

    printf("[WIFI] all client workers stopped\n");

    if(server_thread_created)
    {
        pthread_join(server_thread,NULL);
    }

    /* 所有 worker 退出后，才能销毁 group、队列和设备表。 */
    wifi_worker_group_destroy(&wifi_worker_group);

    /*
     *等待所有的生产中都已经退出之后
     *才能真正的销毁queue
     */
    message_queue_destroy(&upstream_queue);

    device_manager_destroy(&device_manager);

    return 0;
}
