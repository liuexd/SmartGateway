#define _POSIX_C_SOURCE 200809L

#include "gateway_loop.h"
#include "gateway_app.h"
#include "line_parser.h"
#include "wifi_server.h"
#include "wifi_client.h"
#include "bluetooth_worker.h"

#include  <stdlib.h>
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/*
 * 读缓冲大小。
 */
#define GATEWAY_READ_BUFFER_SIZE 256
#define GATEWAY_WIFI_PORT 7000

/*
 * TCP接收累计缓冲区大小。
 */
#define GATEWAY_TCP_BUFFER_SIZE 1024

int gateway_loop_run(
    const char *serial_device,
    int baud_rate,
    gateway_context_t *context,
    frame_parser_t *parser,
    const volatile int *running)
{
    // gateway-server通信专用
    uint8_t read_buffer[GATEWAY_READ_BUFFER_SIZE];
    char tcp_buffer[GATEWAY_TCP_BUFFER_SIZE];
    size_t tcp_length = 0;

    // wifi_node-gateway通信专用
    //后续由于程序改为多线程，导致wifi_buffer、wifi_length、client_fd只属于每个work自己
    int wifi_listen_fd;
    wifi_worker_group_t wifi_workers;

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

    bluetooth_context.serial_device = serial_device;
    bluetooth_context.baud_rate = baud_rate;
    bluetooth_context.gateway_context = context;
    bluetooth_context.parser = parser = parser;
    bluetooth_context.running = running;

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

            wifi_worker_group_destroy(&wifi_workers);

            wifi_server_close(wifi_listen_fd);

            return -1;
        }

        bluetooth_thread_created = 1;
    }

    while (*running)
    {
        /*
            * pollfds[0] = 蓝牙/串口
            * pollfds[1] = GateWay -> Server TCP
            * pollfds[2] = WIFI 节点监听 socket
            * pollfds[3] = 已接入的 WIFI 节点客户端
            */
        struct pollfd pollfds[2];
        nfds_t poll_count;
        int poll_result;

        gateway_app_try_tcp_connect(context);

        pollfds[0].fd = context->tcp_fd;
        pollfds[0].events = POLLIN;
        pollfds[0].revents = 0;

        pollfds[1].fd = wifi_listen_fd;
        pollfds[1].events = POLLIN;
        pollfds[1].revents = 0;

        poll_count = 2;

        poll_result = poll(pollfds, poll_count, 1000);

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

        if ((pollfds[1].revents & POLLIN) != 0)
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
            * 处理TCP数据：服务器下发的命令JSON。
            */
        if (context->tcp_fd >= 0 &&
            (pollfds[0].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
        {
            ssize_t received;

            received = recv(
                context->tcp_fd,
                read_buffer,
                sizeof(read_buffer),
                0);

            if (received > 0)
            {
                size_t received_size = (size_t)received;

                if (tcp_length + received_size >
                    sizeof(tcp_buffer))
                {
                    fprintf(
                        stderr,
                        "[TCP] accumulated receive buffer overflow\n");

                    /*
                        * 丢弃整条消息，等待服务器重新发送。
                        */
                    tcp_length = 0;
                }
                else
                {
                    memcpy(
                        tcp_buffer + tcp_length,
                        read_buffer,
                        received_size);
                    tcp_length += received_size;

                    /*
                        * 按行拆分JSON消息，逐条处理。
                        */
                    line_parser_feed(
                        tcp_buffer,
                        &tcp_length,
                        sizeof(tcp_buffer),
                        gateway_app_on_tcp_line,
                        context);
                }
            }
            else if (received == 0)
            {
                printf("[TCP] server disconnected\n");

                tcp_client_close(context->tcp_fd);
                context->tcp_fd = -1;
                tcp_length = 0;
                context->next_tcp_retry = time(NULL) + 1;
            }
            else if (errno != EINTR)
            {
                fprintf(
                    stderr,
                    "[TCP] recv failed: %s\n",
                    strerror(errno));

                tcp_client_close(context->tcp_fd);
                context->tcp_fd = -1;
                tcp_length = 0;
                context->next_tcp_retry = time(NULL) + 1;
            }
        }
    }

    /*
     * 程序不是因为Ctrl+C退出，
     * 而是串口断开，则等待1秒重新连接。
     */
    if (*running)
    {
        fprintf(stderr, "[IO] retry connection in 1 second\n");
        sleep(1);
    }

    /*
     * 退出主循环后清理WiFi资源。
     */
    wifi_server_close(wifi_listen_fd);

    printf("[BT] waitting for Bluetooth worker...\n");

    if(bluetooth_thread_created)
    {
        pthread_join(bluetooth_thread,NULL);
    }
    printf("[BT] Bluetooth worker stopped\n");

    printf("[WIFI] waiting for client workers...\n");

    wifi_worker_group_wait(&wifi_workers);

    printf("[WIFI] all client workers stopped\n");

    wifi_worker_group_destroy(&wifi_workers);

    return 0;
}
