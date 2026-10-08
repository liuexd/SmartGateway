#include "wifi_client.h"
#include "gateway_app.h"
#include "line_parser.h"
#include "message_json.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#define WIFI_CLIENT_READ_BUFFER_SIZE 256
#define WIFI_CLIENT_BUFFER_SIZE 1024

/*
 * 单次下发命令允许的最长发送时间。
 *
 * 超时后认为这条链路已经不可用，
 * 由调用方走断线处理（回 transport_send_failed 并重新绑定）。
 */
#define WIFI_CLIENT_SEND_TIMEOUT_MS 1000

/*
 * 把 socket 设为非阻塞。
 *
 * 必须这么做，否则 send() 在对端不读、内核发送缓冲写满时
 * 会**无限阻塞**：该 worker 卡死后不再 recv、也不再响应 running，
 * 退出时 wifi_worker_group_wait() 会永久等待，网关无法关闭。
 *
 * 串口侧 serial_port_open() 已经是 O_NONBLOCK，
 * 这里保持两侧行为对称。
 */
static int wifi_client_set_nonblocking(
    int fd
)
{
    int flags;

    if(fd < 0)
    {
        return -1;
    }

    flags = fcntl(fd, F_GETFL, 0);

    if(flags < 0)
    {
        return -1;
    }

    if(fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        return -1;
    }

    return 0;
}

/*
 * 带超时的发送。
 *
 * 实现与 serial_port_write_all() 保持同一套模式：
 *   poll(POLLOUT, timeout) -> write -> 处理部分写/EAGAIN
 *
 * @return 0成功；-1失败（errno 已设置，超时为 ETIMEDOUT）
 */
static int wifi_client_send_all(
    int fd,
    const void *data,
    size_t length
)
{
    const uint8_t *bytes;
    size_t total = 0;

    if(fd < 0 ||
        (data == NULL && length > 0U))
    {
        errno = EINVAL;
        return -1;
    }

    bytes = (const uint8_t *)data;

    /*
     * send()不保证一次写完，必须循环。
     *
     * 注意 errno 只在 send() 返回负值时才有效，
     * 不能写成 sent > 0 && errno == EINTR。
     */
    while(total < length)
    {
        struct pollfd pfd;
        int poll_result;
        ssize_t sent;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        /*
         * 等 socket 可写。超时说明对端长时间不读，
         * 链路已不可用，宁可报失败也不要无限挂着。
         */
        do
        {
            poll_result = poll(
                &pfd,
                1,
                WIFI_CLIENT_SEND_TIMEOUT_MS
            );
        }
        while(poll_result < 0 && errno == EINTR);

        if(poll_result < 0)
        {
            return -1;
        }

        if(poll_result == 0)
        {
            errno = ETIMEDOUT;
            return -1;
        }

        if((pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            errno = EPIPE;
            return -1;
        }

        if((pfd.revents & POLLOUT) == 0)
        {
            continue;
        }
        //MSG_NOSIGNAL可以避免发送时TCP连接断开所导致的程序终止问题
        sent = send(fd, bytes + total, length - total, MSG_NOSIGNAL);

        if(sent > 0)
        {
            total += (size_t)sent;
            continue;
        }

        if(sent < 0 && errno == EINTR)
        {
            continue;
        }

        /*
         * 非阻塞 socket 缓冲区暂时满：
         * 回到 poll 继续等，由它负责超时。
         */
        if(sent < 0 &&
            (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            continue;
        }

        if(sent == 0)
        {
            errno = EPIPE;
        }

        return -1;
    }

    return 0;
}

static int wifi_client_try_send_command(
    device_manager_t *device_manager,
    gateway_app_upstream_context_t *app_context,
    int client_fd
)
{
    frame_command_t command;

    char json[256];

    int result;
    int json_length;

    if(device_manager == NULL ||
        app_context == NULL ||
        client_fd < 0)
    {
        return -1;
    }

    /*
     *这个连接还没有通过第一条合法DATA
     *确认node_id，Device Manager里没有它的条目，
     *此时没有命令可发。
     *
     *注意是 bound_node_id[0]，不是 bound_node_id：
     *数组名不能和字符比较。
     */
    if(app_context->bound_node_id[0] == '\0')
    {
        return 0;
    }

    result = device_manager_try_dequeue_command(
        device_manager,
        app_context->bound_node_id,
        client_fd,
        pthread_self(),
        &command
    );

    /*
     *队列为空：正常情况，回去收节点数据。
     *
     *这里绝不能报错：没有命令是常态，
     *每100ms都会走到这里。
     */
    if(result == DEVICE_MANAGER_QUEUE_EMPTY)
    {
        return 0;
    }

    /*
     *本worker已不再是这个node连接的拥有者
     *（例：节点重连，新worker已接管）。
     *返回-1让外层循环退出，避免旧worker继续乱写。
     */
    if(result == DEVICE_MANAGER_CONFLICT)
    {
        fprintf(stderr,
            "[WIFI CMD] connection ownership conflict, node=%s fd=%d\n",
            app_context->bound_node_id,
            client_fd
        );

        return -1;
    }

    if(result != DEVICE_MANAGER_OK)
    {
        fprintf(stderr,
            "[WIFI CMD] dequeue failed, node=%s result=%d\n",
            app_context->bound_node_id,
            result
        );

        return 0;
    }

    json_length = message_json_build_command(
        json,
        sizeof(json),
        &command
    );

    if (json_length < 0)
    {
        fprintf(
            stderr,
            "[WIFI CMD] JSON build failed, node=%s seq=%06u\n",
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

    if(wifi_client_send_all(client_fd,json,(size_t)json_length)!= 0)
    {
        fprintf(
            stderr,
            "[WIFI CMD] send failed, node=%s fd=%d: %s\n",
            app_context->bound_node_id,
            client_fd,
            strerror(errno)
        );

        gateway_app_enqueue_nack(
            app_context->upstream_queue,
            app_context->bound_node_id,
            command.sequence,
            "transport_send_failed"
        );

        return -1;
    }

    printf(
        "[WIFI CMD] sent node=%s seq=%06u bytes=%d\n",
        app_context->bound_node_id,
        (unsigned int)command.sequence,
        json_length
    );

    return 1;
}

void *wifi_client_worker(void *arg)
{
    wifi_client_context_t *client;

    int client_fd;
    gateway_context_t  *gateway_context;
    const volatile int *running;
    wifi_worker_group_t  *worker_group;
    message_queue_t *upstream_queue;
    gateway_app_upstream_context_t app_context;
    device_manager_t *device_manager;

    uint8_t  read_buffer[WIFI_CLIENT_READ_BUFFER_SIZE];
    char wifi_buffer[WIFI_CLIENT_BUFFER_SIZE];
    size_t wifi_length = 0;

    if(arg == NULL)
    {
        return NULL;
    }

    client = (wifi_client_context_t *)arg;

    /*
     *先把长期需要的数据复制出来
     */
    client_fd = client->client_fd;
    gateway_context = client->gateway_context;
    running = client->running;
    worker_group = client->wifi_worker_group;
    upstream_queue = client ->upstream_queue;
    device_manager = client ->device_manager;

    /*
     * 启动参数结构体已经没有用了。
     * 由 worker 负责释放。
     */

    free(client);

    app_context.gateway_context = gateway_context;
    app_context.upstream_queue = upstream_queue;
    app_context.device_manager = device_manager;
    app_context.transport = DEVICE_TRANSPORT_WIFI;
    app_context.device_fd = client_fd;
    app_context.bound_node_id[0] = '\0';

    /*
     * 必须是非阻塞的：
     *   · send() 满时不能无限挂着（见 wifi_client_send_all）
     *   · recv() 也必须配合 poll 使用，不能直接阻塞
     *
     * 失败则该连接不可靠，直接退出让上层回收。
     */
    if(wifi_client_set_nonblocking(client_fd) != 0)
    {
        fprintf(
            stderr,
            "[WIFI WORKER] cannot set nonblocking, fd=%d: %s\n",
            client_fd,
            strerror(errno)
        );

        close(client_fd);

        wifi_worker_group_remove(worker_group);

        return NULL;
    }

    printf("[WIFI WORKER] started, fd=%d\n", client_fd);

    /*
     *核心程序
     */
    struct pollfd pfd;

    pfd.fd = client_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    while (*running)
    {
        int poll_result;

        pfd.revents = 0;

        {
            int command_result;

            command_result = wifi_client_try_send_command(
                device_manager,
                &app_context,
                client_fd
            );
            if(command_result < 0)
            {
                break;
            }
        }

        poll_result = poll(&pfd,1,100);

        if(poll_result < 0)
        {
            if(errno == EINTR)
            {
                continue;
            }

            fprintf(
                stderr,
                "[WIFI WORKER] poll failed, fd=%d: %s\n",
                client_fd,
                strerror(errno)
            );

            break;
        }

        /*
         *1秒没有数据
         *回到while顶部检查running
         */
        if(poll_result == 0)
        {
            continue;
        }

        if((pfd.revents & POLLIN) != 0)
        {
            ssize_t received;

            received = recv(
                client_fd,
                read_buffer,
                sizeof(read_buffer),
                0
            );

            if(received > 0)
            {
                size_t received_size;

                received_size = (size_t) received;

                if(wifi_length + received_size > sizeof(wifi_buffer))
                {
                    fprintf(stderr,
                    "[WIFI WORKER] receive buffer overflow, fd = %d\n",client_fd);


                wifi_length = 0;
                continue;
                }

                memcpy(
                    wifi_buffer + wifi_length,
                    read_buffer,
                    received_size
                );

                wifi_length += received_size;

                /*
                *TCP没有消息边界
                *继续复用现有的JSON Lines解析器
                */

                line_parser_feed(
                    wifi_buffer,
                    &wifi_length,
                    sizeof(wifi_buffer),
                    gateway_app_on_wifi_line,
                    &app_context
                );

                continue;
            }
            else if(received == 0)
            {
                /*
                 *对端正常关闭连接，不是错误。
                 */
                printf(
                    "[WIFI WORKER] node disconnected, fd=%d\n",
                    client_fd
                );

                break;
            }
            else if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                /*
                 * 非阻塞 socket 上 poll 报可读但数据已被取走，
                 * 属于正常竞争，回顶层重新 poll 即可。
                 */
                continue;
            }
            else if(errno != EINTR)
            {
                fprintf(
                    stderr,
                    "[WIFI WORKER] recv failed, fd=%d: %s\n",
                    client_fd,
                    strerror(errno)
                );

                break;
            }
        }
        if((pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
        {
            printf(
                "[WIFI WORKER] connection closed, fd=%d\n",
                client_fd
            );

            break;
        }
    }

    /*
    * 如果这个连接已经成功识别出node，
    * 通知Device Manager连接已经退出。
    */
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
                client_fd,
                pthread_self(),
                pending_commands,
                DEVICE_COMMAND_QUEUE_CAPACITY,
                &pending_count
            );


        if (disconnect_result ==
            DEVICE_MANAGER_OK)
        {
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
                    "[WIFI CMD] failed %zu pending command(s), "
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
            printf(
                "[WIFI] stale disconnect ignored, node=%s fd=%d\n",
                app_context.bound_node_id,
                client_fd
            );
        }
    }

    /*
     *client_fd 的所有权属于本worker
     */


     close(client_fd);

     printf("[WIFI WORKER] stopped, fd=%d\n", client_fd);

     wifi_worker_group_remove(worker_group);

     return NULL;
}

int wifi_worker_group_init(wifi_worker_group_t *group)
{
    if(group == NULL)
    {
        return -1;
    }

    group->active_workers = 0;
    if(pthread_mutex_init(&group->mutex,NULL)!=0){
        return -1;
    }

    if(pthread_cond_init(&group->all_stopped,NULL)!=0)
    {
        pthread_mutex_destroy(&group->mutex);
        return -1;
    }

    return 0;
}

void wifi_worker_group_add(wifi_worker_group_t *group)
{
    pthread_mutex_lock(&group->mutex);

    group->active_workers++;

    pthread_mutex_unlock(&group->mutex);
}

void wifi_worker_group_remove(
    wifi_worker_group_t *group
)
{
    pthread_mutex_lock(&group->mutex);

    if(group->active_workers>0)
    {
        group->active_workers--;
    }
    if(group->active_workers==0)
    {
        pthread_cond_broadcast(&group->all_stopped);
    }

    pthread_mutex_unlock(&group->mutex);
}

void wifi_worker_group_wait(
    wifi_worker_group_t *group
)
{
    pthread_mutex_lock(&group->mutex);

    while(group->active_workers >0)
    {
        pthread_cond_wait(
            &group->all_stopped,
            &group->mutex
        );
    }

    pthread_mutex_unlock(&group->mutex);
}

void wifi_worker_group_destroy(
    wifi_worker_group_t *group
)
{
    pthread_cond_destroy(&group->all_stopped);

    pthread_mutex_destroy(&group->mutex);
}