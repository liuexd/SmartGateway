#define _POSIX_C_SOURCE 200809L

#include "gateway_loop.h"
#include "gateway_app.h"
#include "line_parser.h"

#include <errno.h>
#include <poll.h>
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

/*
 * TCP接收累计缓冲区大小。
 */
#define GATEWAY_TCP_BUFFER_SIZE 1024

int gateway_loop_run(
    const char *serial_device,
    int baud_rate,
    gateway_context_t *context,
    frame_parser_t *parser,
    const volatile int *running
)
{
    uint8_t read_buffer[GATEWAY_READ_BUFFER_SIZE];
    char tcp_buffer[GATEWAY_TCP_BUFFER_SIZE];
    size_t tcp_length = 0;

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
    while (*running)
    {
        int serial_fd;
        int disconnected = 0;

        serial_fd = serial_port_open(
            serial_device,
            baud_rate
        );

        if (serial_fd < 0)
        {
            fprintf(
                stderr,
                "[IO] cannot open %s: %s; retry in 1 second\n",
                serial_device,
                strerror(errno)
            );

            sleep(1);
            continue;
        }

        context->serial_connections++;

        /*
         * 重新连接后丢弃上一次断线残留的半帧，
         * 但保留统计信息。
         */
        frame_parser_reset(parser);

        printf(
            "[IO] serial connected: %s, connection=%lu\n",
            serial_device,
            context->serial_connections
        );

        /*
         * 记录当前串口fd，供应用层下发命令。
         */
        context->serial_fd = serial_fd;

        /*
         * 内层循环负责正常读取。
         *
         * 串口和TCP同时监听：
         *   串口可读 -> 解析协议帧（DATA/ACK/NACK）并上报服务器；
         *   TCP可读  -> 解析服务器下发的命令JSON，组帧后写串口。
         */
        while (*running && !disconnected)
        {
            struct pollfd pollfds[2];
            nfds_t poll_count;
            int poll_result;

            gateway_app_try_tcp_connect(context);

            pollfds[0].fd = serial_fd;
            pollfds[0].events = POLLIN;
            pollfds[0].revents = 0;
            poll_count = 1;

            if (context->tcp_fd >= 0)
            {
                pollfds[1].fd = context->tcp_fd;
                pollfds[1].events = POLLIN;
                pollfds[1].revents = 0;
                poll_count = 2;
            }

            poll_result = poll(pollfds, poll_count, 1000);

            if (poll_result < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                fprintf(stderr, "[IO] poll failed: %s\n", strerror(errno));
                disconnected = 1;
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

            /*
             * 处理TCP数据：服务器下发的命令JSON。
             */
            if (context->tcp_fd >= 0 &&
                (pollfds[1].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
            {
                ssize_t received;

                received = recv(
                    context->tcp_fd,
                    read_buffer,
                    sizeof(read_buffer),
                    0
                );

                if (received > 0)
                {
                    size_t received_size = (size_t)received;

                    if (tcp_length + received_size >
                        sizeof(tcp_buffer))
                    {
                        fprintf(
                            stderr,
                            "[TCP] accumulated receive buffer overflow\n"
                        );

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
                            received_size
                        );
                        tcp_length += received_size;

                        /*
                         * 按行拆分JSON消息，逐条处理。
                         */
                        line_parser_feed(
                            tcp_buffer,
                            &tcp_length,
                            sizeof(tcp_buffer),
                            gateway_app_on_tcp_line,
                            context
                        );
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
                        strerror(errno)
                    );

                    tcp_client_close(context->tcp_fd);
                    context->tcp_fd = -1;
                    tcp_length = 0;
                    context->next_tcp_retry = time(NULL) + 1;
                }
            }

            /*
             * 处理串口数据：节点上报的协议帧。
             */
            if ((pollfds[0].revents & POLLIN) != 0)
            {
                ssize_t read_length;

                read_length = serial_port_read(
                    serial_fd,
                    read_buffer,
                    sizeof(read_buffer)
                );

                if (read_length > 0)
                {
                    /*
                     * 不能把read_buffer当字符串处理。
                     * 它不一定带'\0'，也不一定是一整帧。
                     */
                    frame_parser_feed(
                        parser,
                        read_buffer,
                        (size_t)read_length,
                        gateway_app_on_frame,
                        context
                    );
                }
                else if (
                    read_length ==
                    SERIAL_READ_WOULD_BLOCK
                )
                {
                    /*
                     * poll之后数据被其他事件消耗，
                     * 或发生竞争，不属于错误。
                     */
                    continue;
                }
                else if (read_length == 0)
                {
                    fprintf(stderr, "[IO] serial device reached EOF\n");
                    disconnected = 1;
                }
                else
                {
                    fprintf(stderr, "[IO] read failed: %s\n", strerror(errno));
                    disconnected = 1;
                }
            }

            /*
             * 串口挂断/错误时退出内层循环，
             * 由外层循环负责重连。
             */
            if ((pollfds[0].revents &
                 (POLLHUP | POLLERR | POLLNVAL)) != 0)
            {
                fprintf(stderr, "[IO] serial device disconnected\n");
                disconnected = 1;
            }
        }

        context->serial_fd = -1;
        serial_port_close(serial_fd);

        /*
         * 程序不是因为Ctrl+C退出，
         * 而是串口断开，则等待1秒重新连接。
         */
        if (*running)
        {
            fprintf(stderr, "[IO] retry connection in 1 second\n");
            sleep(1);
        }
    }

    return 0;
}
