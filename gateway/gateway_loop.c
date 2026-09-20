#define _POSIX_C_SOURCE 200809L

#include "gateway_loop.h"
#include "gateway_app.h"
#include "wifi_server.h"
#include "wifi_client.h"
#include "bluetooth_worker.h"
#include "message_queue.h"
#include "server_link.h"

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
    wifi_worker_group_t wifi_workers;

    message_queue_t upstream_queue;

    //北向连接服务器管理线程
    pthread_t server_thread;
    server_link_context_t server_context;
    int server_thread_created = 0;

    //蓝牙节点
    pthread_t bluetooth_thread;
    bluetooth_worker_context_t bluetooth_context;
    int bluetooth_thread_created = 0;

    if (serial_device == NULL ||
        context == NULL ||
        parser == NULL ||
        running == NULL)
    {
        return -1;
    }

    printf("Gateway started\n");
    printf("Serial device: %s\n", serial_device);
    printf("Baud rate    : %d, 8N1, raw mode\n", baud_rate);

    /*
     * 外层循环负责串口断线重连。
     */

    wifi_listen_fd = wifi_server_open(GATEWAY_WIFI_PORT);

    if (wifi_listen_fd < 0)
    {
        fprintf(stderr, "[WIFI] cannot listen on port %d: %s\n", GATEWAY_WIFI_PORT, strerror(errno));
        return -1;
    }

    printf("[WIFI] listening on port %d\n", GATEWAY_WIFI_PORT);

    if(wifi_worker_group_init(&wifi_workers)!=0)
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

            wifi_worker_group_destroy(&wifi_workers);

            wifi_server_close(wifi_listen_fd);

            return -1;
    }

    server_context.gateway_context = context;
    server_context.upstream_queue = &upstream_queue;
    server_context.running = running;

    {
        int  pthread_result;

        pthread_result = pthread_create(&server_thread,NULL,
            server_link_worker,&server_context);
        if(pthread_result != 0)
        {
            fprintf(stderr,
                "[SERVER LINK] pthread_create failed: %s\n",
                strerror(pthread_result));

                message_queue_destroy(&upstream_queue);

                wifi_worker_group_destroy(&wifi_workers);

                wifi_server_close(wifi_listen_fd);

                return -1;
        }

        server_thread_created = 1;
    }

    bluetooth_context.serial_device = serial_device;
    bluetooth_context.baud_rate = baud_rate;
    bluetooth_context.gateway_context = context;
    bluetooth_context.parser = parser;
    bluetooth_context.running = running;
    bluetooth_context.upstream_queue = &upstream_queue;

    {
        int pthread_result;
        pthread_result = pthread_create(
            &bluetooth_thread,
            NULL,
            bluetooth_worker,
            &bluetooth_context
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

            wifi_worker_group_destroy(&wifi_workers);

            wifi_server_close(wifi_listen_fd);

            return -1;
        }

        bluetooth_thread_created = 1;
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
        struct pollfd pollfds;
        int poll_result;

        pollfds.fd = wifi_listen_fd;
        pollfds.events = POLLIN;
        pollfds.revents = 0;


        poll_result = poll(&pollfds, 1, 1000);

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

        if ((pollfds.revents & POLLIN) != 0)
        {
            int new_client_fd;

            new_client_fd = wifi_server_accept(wifi_listen_fd);
            if (new_client_fd < 0)
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
                pthread_t thread;
                int pthread_result;
                //
                client = malloc(sizeof(*client));

                if(client == NULL)
                {
                    fprintf(stderr,
                        "[WIFI] malloc client context failed\n");
                        wifi_server_close(new_client_fd);

                }
                else{
                    client->client_fd = new_client_fd;
                    client->gateway_context = context;
                    client->running = running;
                    client->wifi_worker_group = &wifi_workers;
                    client->upstream_queue = &upstream_queue;

                    wifi_worker_group_add(&wifi_workers);

                    pthread_result = pthread_create(
                        &thread,
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

                        wifi_server_close(new_client_fd);
                        free(client);
                        wifi_worker_group_remove(&wifi_workers);
                    }
                    else{
                        printf("[WIFI] worker create, fd=%d\n",new_client_fd);

                        pthread_result = pthread_detach(thread);

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


        /*
            * 处理TCP数据：服务器下发的命令JSON。已经由server_link负责接收任务
            */
    }

    /*
     * 退出主循环后清理WiFi资源。不再接收新的wifi节点进行连接
     */
    wifi_server_close(wifi_listen_fd);

    printf("[BT] waitting for Bluetooth worker...\n");

    /*
     *非常重要
     *先唤醒在queue_push当中的Worker
     */
    message_queue_shutdown(&upstream_queue);

    if(bluetooth_thread_created)
    {
        pthread_join(bluetooth_thread,NULL);
    }
    printf("[BT] Bluetooth worker stopped\n");

    printf("[WIFI] waiting for client workers...\n");

    wifi_worker_group_wait(&wifi_workers);

    printf("[WIFI] all client workers stopped\n");

    if(server_thread_created)
    {
        pthread_join(server_thread,NULL);
    }

    wifi_worker_group_destroy(&wifi_workers);

    /*
     *等待所有的生产中都已经退出之后
     *才能真正的销毁queue
     */
    message_queue_destroy(&upstream_queue);

    return 0;
}
