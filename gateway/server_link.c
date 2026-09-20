#define _POSIX_C_SOURCE 200809L

#include "server_link.h"

#include "gateway_app.h"
#include "line_parser.h"
#include "tcp_client.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#define SERVER_LINK_READ_BUFFER_SIZE 256
#define SERVER_LINK_BUFFER_SIZE 1024

/*
 *队列为空时最多等待这么久
 *然后重新检查queue
 */
#define SERVER_LINK_POLL_TIMEOUT_MS 100

//北向断开服务器
static void server_link_disconnect(
    gateway_context_t *context,
    size_t *tcp_length
)
{
    time_t now;

    if(context->tcp_fd >= 0)
    {
        tcp_client_close(context->tcp_fd);

        context->tcp_fd = -1;
    }

    if(tcp_length != NULL)
    {
        *tcp_length = 0;
    }

    now = time(NULL);

    if(now !=(time_t)-1)//检查是否失败
    {
        context->next_tcp_retry = now + 1;
    }
}

static void server_link_short_wait(void)
{
    struct timespec delay;
    delay.tv_sec =0;
    delay.tv_nsec = 100000000L;

    nanosleep (&delay,NULL);
}

void *server_link_worker(void *arg)
{
    server_link_context_t *worker;

    gateway_context_t *context;
    message_queue_t *queue;
    const volatile int *runningg;

    gateway_message_t pending_messsage;
    int have_pending_message = 0;

    uint8_t read_buffer[SERVER_LINK_READ_BUFFER_SIZE];

    char tcp_buffer[SERVER_LINK_BUFFER_SIZE];

    size_t tcp_length =0;

    if(arg == NULL)
    {
        return NULL;
    }

    worker =(server_link_context_t *)arg;

    context = worker->gateway_context;
    queue = worker->upstream_queue;
    runningg = worker->running;

    if(worker->gateway_context ==NULL||
        worker->upstream_queue == NULL||
        worker->running ==NULL)
    {
        return NULL;
    }

    printf("[SERVER WORKER] started\n");

    while(*runningg)
    {
        int queue_result;
        int sent_message =0;

        /*
         * -------------------------------------------------
         * 1. 尝试从 upstream queue 取一条消息。
         *
         * 不使用阻塞 pop，
         * 因为还需要处理 Server 下行数据。
         * -------------------------------------------------
         */
        if(!have_pending_message)
        {
            queue_result = message_queue_try_pop(queue,&pending_messsage);

            if(queue_result == MESSAGE_QUEUE_OK)
            {
                have_pending_message = 1;
            }
            else if(queue_result == MESSAGE_QUEUE_SHUTDOWN)
            {
                break;
            }
            else if(queue_result != MESSAGE_QUEUE_EMPTY)
            {
                fprintf(stderr,
                    "[SERVER WORKER] queue pop failed\n");
                
                    break;
            }
        }

        /*
         * -------------------------------------------------
         * 2. Server未连接则尝试连接。
         *
         * 真正的1秒重试节流仍复用
         * gateway_app_try_tcp_connect()。
         * -------------------------------------------------
         */

        if(context->tcp_fd <0)
        {
            gateway_app_try_tcp_connect(
                context
            );

            if(context->tcp_fd < 0)
            {
                server_link_short_wait();
                continue;
            }

            /*
             *新连接建立，丢弃上一次残留的半条JSON。
             */
            tcp_length = 0;
        }

                /*
         * -------------------------------------------------
         * 3. 有待发送消息时统一由 Server Worker 发送。
         * -------------------------------------------------
         */

        if(have_pending_message)
        {
            if(tcp_client_send_all(
                context->tcp_fd,
                pending_messsage.data,
                pending_messsage.length
                ) != 0
            )
            {
                context->tcp_send_error++;

                fprintf(stderr,
                    "[SERVER WORKER] send failed: %s\n",
                    strerror(errno)
                );

                /*
                * 注意：
                * pending_message 不清除。
                *
                * reconnect成功以后首先重新发送这一条。
                */
                server_link_disconnect(context,&tcp_length);

                continue;
            }

            context->tcp_send++;

            printf("[SERVER WORKER] sent, bytes=%u\n",
                (unsigned int)pending_messsage.length);

            /*
             *只有发送成功才清空 pending，
             *否则重连后重发同一条。
             */
            have_pending_message = 0;
            sent_message = 1;
        }

        /*
        * -------------------------------------------------
        * 4. 检查 Server 是否发来了 CMD。
        *
        * 如果刚发送了一条上行消息：
        *     timeout=0
        *     只快速检查一下，不阻塞继续消费queue。
        *
        * 如果当前没有上行消息：
        *     timeout=100ms
        *     等待Server下行。
        * -------------------------------------------------
        */
        {
            struct pollfd pfd;
            int poll_result;
            int timeout_ms;

            pfd.fd = context->tcp_fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            timeout_ms = sent_message ? 0 :SERVER_LINK_POLL_TIMEOUT_MS;

            poll_result = poll(&pfd,1,timeout_ms);

            if(poll_result < 0)
            {
                if(errno == EINTR)
                {
                    continue;
                }

                fprintf(stderr,
                "[SERVER WORKER] poll failed: %s\n",
                strerror(errno));

                server_link_disconnect(context,&tcp_length);

                continue;
            }

            if(poll_result == 0)
            {
                continue;
            }

            if((pfd.revents & POLLIN)!= 0)
            {
                ssize_t received;
                received = recv(context->tcp_fd,read_buffer,sizeof(read_buffer),0);
                if(received > 0)
                {
                    size_t received_size;
                    received_size = (size_t)received;

                    if(tcp_length + received_size > sizeof(tcp_buffer))
                    {
                        fprintf(stderr,
                        "[SERVER WORKER] receive buffer overflow\n");

                        tcp_length = 0;
                    }
                    else
                    {
                        memcpy(tcp_buffer + tcp_length,
                            read_buffer,
                            received_size);
                        
                        tcp_length += received_size;
                        
                        line_parser_feed(tcp_buffer,
                            &tcp_length,
                            sizeof(tcp_buffer),
                            gateway_app_on_tcp_line,
                            context
                        );
                    }
                }
                else if (received == 0)
                {
                    printf("[SERVER WORKER] server disconnected\n");

                    server_link_disconnect(context,&tcp_length);

                    continue;
                }
                else if (errno != EINTR)
                {
                    fprintf(stderr,
                        "[SERVER WORKER] recv failed: %s\n",
                        strerror(errno));

                    server_link_disconnect(context,&tcp_length);

                    continue;
                }

            }

            /*
             * Server socket挂断或异常。
             */
            if(context->tcp_fd >= 0 && (pfd.revents &
                (POLLHUP | POLLERR |POLLNVAL))!=0)
            {
                fprintf(stderr,
                "[SERVER WORKER] connection lost\n");

                server_link_disconnect(context,&tcp_length);
            }
        }
    }

    if(context->tcp_fd >= 0)
    {
        tcp_client_close (context->tcp_fd);

        context->tcp_fd = -1;
    }
    printf("[SERVER WORKER] stopped\n");
    return NULL;
}