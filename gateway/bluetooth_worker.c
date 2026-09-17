#include "bluetooth_worker.h"

#include "serial_port.h"
#include "gateway_app.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BLUETOOTH_READ_BUFFER_SIZE 256

#include <stdio.h>

void *bluetooth_worker(void *arg)
{
    bluetooth_worker_context_t *worker;

    const char *serial_device;
    int baud_rate;

    gateway_context_t *context;
    frame_parser_t *parser;
    const volatile int *running;

    uint8_t read_buffer[BLUETOOTH_READ_BUFFER_SIZE];

    if(arg == NULL)
    {
        return NULL;
    }

    worker = (bluetooth_worker_context_t *) arg;

    serial_device = worker->serial_device;
    baud_rate = worker->baud_rate;
    context = worker->gateway_context;
    parser =worker->parser;
    running = worker->running;

    if(serial_device == NULL ||
        context == NULL ||
        parser == NULL ||
        running == NULL
    )
    {
        return NULL;
    }

    printf("[BT WORKER] started: device=%s baud=%d\n",
        worker->serial_device,
        worker->baud_rate
    );

    while(*running)
    {
        int  serial_fd;
        int disconnected = 0;

        serial_fd = serial_port_open(
            serial_device,
            baud_rate
        );

        if(serial_fd < 0)
        {
            fprintf(
                stderr,
                "[BT WORKER] cannot open %s: %s;retry in 1 second\n",
                serial_device,
                strerror(errno)
            );

            sleep(1);
            continue;
        }

        context->serial_connections++;

        /*
         *重连以后丢弃上一次残留的半帧。
         */
        frame_parser_reset(parser);

        /*
         *目前仍然保存在gateway_context中，
         *供原有下行CMD路径使用
         */

        context->serial_fd = serial_fd;

        printf("[BT WORKER] serial connected: %s, connection=%lu\n",
            serial_device,
            context->serial_connections
        );

        /*
         *内层循环
         *负责这个serial_fd的正常接收
         */
        while(*running && !disconnected)
        {
            struct pollfd pfd;
            int poll_result;

            pfd.fd = serial_fd;
            pfd.revents = 0;
            pfd .events = POLLIN;

            poll_result = poll(&pfd,
                1,1000
            );

            if(poll_result < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }
                fprintf(stderr,
                    "[BT WORKER] poll failed: %s\n",
                    strerror(errno)
                );

                disconnected = 1;
                continue;
            }

            /*
             *超时不是错误
             *回到while顶部检查running。
             */

             if(poll_result == 0)
             {
                continue;
             }

             if((pfd.revents & POLLIN)!=0)
             {
                ssize_t read_length;

                read_length = serial_port_read(
                    serial_fd,
                    read_buffer,
                    sizeof(read_buffer)
                );

                if(read_length >0)
                {
                    frame_parser_feed(
                        parser,
                        read_buffer,
                        (size_t)read_length,
                        gateway_app_on_frame,
                        context
                    );
                }
                else if(
                    read_length == SERIAL_READ_WOULD_BLOCK
                )
                {
                    continue;
                }
                else if(read_length == 0)
                {
                    fprintf(
                        stderr,
                        "[BT WORKER] serial device reached EOF\n "
                    );

                    disconnected = 1;
                }

                else{
                    fprintf(stderr,
                        "[BT WORKER] read failed: %s\n",
                        strerror(errno)
                    );

                    disconnected = 1;
                }
            }

            if((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
            {
                fprintf(
                    stderr,
                    "[BT WORKER] serial device disconnected\n"
                );

                disconnected = 1;
            }
        }

        context->serial_fd = -1;

        serial_port_close(
            serial_fd
        );

        /*
         *Ctrl+C时不需要重连
         */

         if(*running)
         {
            fprintf(stderr,
                "[BT WORKER] retry connect in 1 second\n"
            );

            sleep(1);
         }
    }

    context->serial_fd = -1;

    printf("[BT WORKER] stopped\n");

    return NULL;
}