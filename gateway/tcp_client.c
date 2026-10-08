#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "tcp_client.h"

#include <stdio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * 单次发送允许的最长等待时间。
 *
 * 对端长时间不读时，send() 会因内核发送缓冲写满而阻塞；
 * 如果不设上限，server_link_worker 就会被卡死：
 * 既不再消费 upstream_queue，也不再轮询下行 CMD，
 * 整个北向上报随之中断。
 */
#define TCP_CLIENT_SEND_TIMEOUT_MS 1000

/*
 * 把 socket 置为非阻塞。
 *
 * 仅当 connect 之后调用；不改变对外接口语义。
 */
int tcp_client_set_nonblocking(int socket_fd)
{
    int flags;

    if(socket_fd < 0)
    {
        errno = EINVAL;
        return -1;
    }

    flags = fcntl(socket_fd, F_GETFL, 0);

    if(flags < 0)
    {
        return -1;
    }

    if(fcntl(socket_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        return -1;
    }

    return 0;
}

int tcp_client_connect(const char *server_ip,uint16_t port)
{
    if(server_ip==NULL||server_ip[0]=='\0')
    {
        errno =EINVAL;
        return -1;
    }

    int client_socket_fd = -1;
    struct sockaddr_in serveraddr;

    client_socket_fd = socket(AF_INET,SOCK_STREAM,0);
    if(client_socket_fd<0)
    {
        int save_error = errno;
        close(client_socket_fd);
        errno = save_error;
        return -1;
    }
    memset(&serveraddr,0,sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    //serveraddr.sin_addr.s_addr = inet_addr(server_ip);
    if(!inet_aton(server_ip,&serveraddr.sin_addr))
    {
        printf("invalid server_ip\n");
        errno = EINVAL;
        close(client_socket_fd);
        return -1;
    }
    serveraddr.sin_port = htons(port);
    int ret=connect(client_socket_fd,(const struct sockaddr*)&serveraddr,sizeof(serveraddr));
    if(ret<0)
    {
        int save_error = errno;
        close(client_socket_fd);
        errno = save_error;
        return -1;
    }
    return client_socket_fd;
}

int tcp_client_send_all(int socket_fd,const void* data,size_t length)
{
    const uint8_t *bytes;
    size_t total_send = 0;
    if(socket_fd<0 ||(data == NULL&&length > 0U))
    {
        errno = EINVAL;
        return -1;
    }
    bytes = (const uint8_t *)data;

    while(total_send < length)
    {
        struct pollfd pfd;
        int poll_result;
        ssize_t retsize;

        /*
         * 先等 socket 可写。
         *
         * 超时说明对端长时间不读，链路已不可用；
         * 宁可报失败让上层重连，也不要无限挂着。
         */
        pfd.fd = socket_fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        do
        {
            poll_result = poll(
                &pfd,
                1,
                TCP_CLIENT_SEND_TIMEOUT_MS
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

        //MSG_NOSIGNAL防止产生SGIPIPE信号会导致进程被杀
        retsize = send(socket_fd,bytes+total_send,length - total_send,
            #ifdef MSG_NOSIGNAL
                        MSG_NOSIGNAL
            #else
                        0
            #endif
        );
        if(retsize>0)
        {
            total_send +=retsize;
            continue;
        }

        //EINTR被打断
        if(retsize<0&&errno == EINTR)
        {
            continue;
        }

        /*
         * 非阻塞 socket 缓冲区暂时满：
         * 回到 poll 继续等，由它负责超时。
         */
        if(retsize<0&&(errno == EAGAIN||errno == EWOULDBLOCK))
        {
            continue;
        }

        if(retsize== 0)
        {
            errno =EPIPE;//对端关闭连接
        }
        return -1;//默认失败出口
    }

    return 0;
}
void tcp_client_close(int socketfd)
{
    if(socketfd>=0)
    {
        close(socketfd);
    }
}