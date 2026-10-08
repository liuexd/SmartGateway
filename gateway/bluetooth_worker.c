#include "bluetooth_worker.h"

#include "serial_port.h"
#include "gateway_app.h"
#include "frame.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BLUETOOTH_READ_BUFFER_SIZE 256

static int bluetooth_worker_try_send_command(
    device_manager_t *device_manager,
    gateway_app_upstream_context_t *app_context,
    int serial_fd
)
{
    frame_command_t command;

    char frame[FRAME_MAX_LEN];

    int result;
    int frame_length;

    if(device_manager == NULL ||
        app_context == NULL ||
        serial_fd < 0 )
    {
        return -1;
    }

    /*
     * 还没有绑定node_id：这条串口连接的身份尚未确立，
     * Device Manager里没有对应的命令队列，
     * 此时没有命令可发，直接回外层继续收帧。
     *
     * 注意不要在这里报错：绑定之前每秒都会走到这里，
     * 报错会变成刷屏。
     */
    if(app_context->bound_node_id[0] == '\0')
    {
        return 0;
    }

    result = device_manager_try_dequeue_command(
        device_manager,
        app_context->bound_node_id,
        serial_fd,
        pthread_self(),
        &command
    );

    if(result == DEVICE_MANAGER_QUEUE_EMPTY)
    {
        return 0;
    }
    /*
     * 当前Worker已经不是这个设备连接的拥有者。
     */
    if (result ==
        DEVICE_MANAGER_CONFLICT)
    {
        fprintf(
            stderr,
            "[BT CMD] connection ownership conflict, node=%s fd=%d\n",
            app_context->bound_node_id,
            serial_fd
        );

        return -1;
    }


    if (result !=
        DEVICE_MANAGER_OK)
    {
        fprintf(
            stderr,
            "[BT CMD] dequeue failed, node=%s result=%d\n",
            app_context->bound_node_id,
            result
        );

        return 0;
    }

    frame_length = frame_build_command_kv(
        frame,
        sizeof(frame),
        command.sender,
        command.sequence,
        command.fields,
        command.field_count
    );

    if (frame_length < 0)
    {
        fprintf(
            stderr,
            "[BT CMD] frame build failed, node=%s seq=%06u\n",
            app_context->bound_node_id,
            (unsigned int)command.sequence
        );

        gateway_app_enqueue_nack(
            app_context->upstream_queue,
            app_context->bound_node_id,
            command.sequence,
            "cmd_encode_failed"
        );
        return 0;
    }

    if(serial_port_write_all(
        serial_fd,
        frame,
        (size_t)frame_length,
        1000
        )!=0)
    {
        fprintf(
            stderr,
            "[BT CMD] serial write failed, node=%s fd=%d: %s\n",
            app_context->bound_node_id,
            serial_fd,
            strerror(errno)
        );

    gateway_app_enqueue_nack(
        app_context->upstream_queue,
        app_context->bound_node_id,
        command.sequence,
        "transport_send_failed"
    );
        /*
         * 写失败通常意味着当前串口连接已经有问题。
         * 返回-1，让外层连接循环执行断线处理。
         */
        return -1;
    }

    printf(
        "[BT CMD] sent node=%s seq=%06u bytes=%d\n",
        app_context->bound_node_id,
        (unsigned int)command.sequence,
        frame_length
    );

    return 1;
}


void *bluetooth_worker(void *arg)
{
    bluetooth_worker_context_t *worker;

    const char *serial_device;
    int baud_rate;

    gateway_context_t *context;
    frame_parser_t *parser;
    const volatile int *running;
    message_queue_t *upstream_queue;
    gateway_app_upstream_context_t app_context;
    device_manager_t *device_manager;

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
    upstream_queue = worker->upstream_queue;
    device_manager = worker->device_manager;

    if(serial_device == NULL ||
        context == NULL ||
        parser == NULL ||
        upstream_queue == NULL ||
        device_manager == NULL ||
        running == NULL
    )
    {
        return NULL;
    }

    printf("[BT WORKER] started: device=%s baud=%d\n",
        worker->serial_device,
        worker->baud_rate
    );

    app_context.gateway_context = context;
    app_context.upstream_queue = upstream_queue;
    app_context.device_manager = device_manager;
    app_context.transport = DEVICE_TRANSPORT_BLUETOOTH;
    app_context.device_fd = -1;
    app_context.bound_node_id[0] = '\0';

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

        app_context.device_fd = serial_fd;

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

            {
                int command_result;

                command_result = bluetooth_worker_try_send_command(
                    device_manager,
                    &app_context,
                    serial_fd
                );

                if(command_result < 0)
                {
                    disconnected = 1;
                    continue;
                }
            }
            poll_result = poll(&pfd,
                1,100
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
                        &app_context
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

    if (app_context.bound_node_id[0] != '\0')
    {
        frame_command_t pending_commands[
            DEVICE_COMMAND_QUEUE_CAPACITY
        ];

        size_t pending_count;
        size_t i;

        int disconnect_result;


        pending_count = 0U;


        disconnect_result =
            device_manager_disconnect_collect_commands(
                device_manager,
                app_context.bound_node_id,
                serial_fd,
                pthread_self(),
                pending_commands,
                DEVICE_COMMAND_QUEUE_CAPACITY,
                &pending_count
            );


        if (disconnect_result ==
            DEVICE_MANAGER_OK)
        {
            /*
            * 当前连接剩余的命令都已经不可能
            * 通过这个连接发送。
            *
            * 逐条通知Server。
            */
            for (i = 0U;
                i < pending_count;
                i++)
            {
                gateway_app_enqueue_nack(
                    app_context.upstream_queue,
                    pending_commands[i].target_node,
                    pending_commands[i].sequence,
                    "transport_disconnected"
                );
            }


            if (pending_count > 0U)
            {
                printf(
                    "[BT CMD] failed %zu pending command(s), "
                    "node=%s\n",
                    pending_count,
                    app_context.bound_node_id
                );
            }
        }
        else if (
            disconnect_result ==
            DEVICE_MANAGER_CONFLICT)
        {
            /*
            * 说明新连接已经接管。
            *
            * 旧Worker不能碰新连接queue，
            * 也不能为新queue里的命令生成NACK。
            */
            printf(
                "[BT] stale disconnect ignored, node=%s fd=%d\n",
                app_context.bound_node_id,
                serial_fd
            );
            }
    }

        context->serial_fd = -1;

        serial_port_close(
            serial_fd
        );

        /*
         *下一次serial_port_open()
         *对应的新的物理连接生命周期。
         *
         * 必须重新等待第条合法帧确认身份
         */
        app_context.bound_node_id[0] = '\0';

        app_context.device_fd = -1;
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