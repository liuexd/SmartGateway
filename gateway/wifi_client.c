#include "wifi_client.h"
#include "gateway_app.h"
#include "line_parser.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <poll.h>

#define WIFI_CLIENT_READ_BUFFER_SIZE 256
#define WIFI_CLIENT_BUFFER_SIZE 1024

void *wifi_client_worker(void *arg)
{
    wifi_client_context_t *client;

    int client_fd;
    gateway_context_t  *gateway_context;
    const volatile int *running;
    wifi_worker_group_t  *worker_group;


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

    /*
     * 启动参数结构体已经没有用了。
     * 由 worker 负责释放。
     */

    free(client);

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

        poll_result = poll(&pfd,1,1000);

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
                    gateway_context
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
    

    // while(*running)
    // {
    //     ssize_t received;

    //     received = recv(
    //         client_fd,
    //         read_buffer,
    //         sizeof(read_buffer),
    //         0
    //     );

    //     if(received > 0)
    //     {
    //         size_t received_size;

    //         received_size = (size_t) received;

    //         if(wifi_length + received_size > sizeof(wifi_buffer))
    //         {
    //             fprintf(stderr,
    //             "[WIFI WORKER] receive buffer overflow, fd = %d\n",client_fd);
            

    //         wifi_length = 0;
    //         continue;     
    //         }
            
    //         memcpy(
    //             wifi_buffer + wifi_length,
    //             read_buffer,
    //             received_size
    //         );

    //         wifi_length += received_size;

    //         /*
    //          *TCP没有消息边界
    //          *继续复用现有的JSON Lines解析器
    //          */

    //         line_parser_feed(
    //             wifi_buffer,
    //             &wifi_length,
    //             sizeof(wifi_buffer),
    //             gateway_app_on_wifi_line,
    //             gateway_context
    //         );

    //         continue;
    //     }

    //     if(received == 0)
    //     {
    //         printf(
    //             "[WIFI  WORKER] node disconnected, fd=%d\n",
    //             client_fd
    //         );

    //         break;
    //     }

    //     /*
    //      *recv被信号打断，不视为连接错误
    //      */
    //     if(errno == EINTR)
    //     {
    //         continue;
    //     }

    //     fprintf(
    //         stderr,
    //         "[WIFI WORKER] recv failed, fd=%d: %s\n",
    //         client_fd,
    //         strerror(errno)
    //     );

    //     break;
 
    // }

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