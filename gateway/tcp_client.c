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
#include <time.h>
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
 * 单次 connect() 允许的最长等待时间。
 *
 * 阻塞式 connect() 在"对端不回 SYN-ACK"时会一直挂到内核重传
 * 次数耗尽（实测约 18~21 秒）。这会带来两个后果：
 *   1) server_link_worker 长时间停摆，无法消费队列、无法轮询下行；
 *   2) SIGTERM 之后线程仍卡在 connect 里，进程无法及时退出。
 * 因此改成有界 connect，超时后主动放弃并交给上层按 1 秒节奏重试。
 */
#ifndef TCP_CLIENT_CONNECT_TIMEOUT_MS
#define TCP_CLIENT_CONNECT_TIMEOUT_MS 3000
#endif

/*
 * 有界 connect 的轮询分片。
 *
 * 每个分片醒来都会重新检查超时与停止条件，使退出响应粒度
 * 不依赖于整个 connect 超时长度。
 */
#define TCP_CLIENT_CONNECT_POLL_SLICE_MS 100

/* 单调时钟（毫秒），不受系统时间调整影响 */
static long long tcp_client_monotonic_ms(void)
{
    struct timespec now;

    if(clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return -1;
    }

    return (long long)now.tv_sec * 1000LL +
           (long long)now.tv_nsec / 1000000LL;
}

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

/* 恢复阻塞语义：内部实现需要，不属于对外接口 */
static int tcp_client_set_blocking(int socket_fd)
{
    int flags = fcntl(socket_fd, F_GETFL, 0);

    if(flags < 0)
    {
        return -1;
    }

    return fcntl(socket_fd, F_SETFL, flags & ~O_NONBLOCK);
}

/*
 * 有界 connect 的内部实现。
 *
 * timeout_ms  : 单次 connect 允许等待的总时长
 * running     : 可为 NULL。非 NULL 时每个轮询分片检查一次，
 *               置 0 则立刻放弃连接并返回 EINTR。
 *
 * 返回 0 成功（socket 恢复为阻塞模式，与旧接口行为一致），
 * 返回 -1 失败且 errno 已设置，调用者负责关闭 socket。
 */
static int tcp_client_connect_bounded(
    int socket_fd,
    const struct sockaddr_in *address,
    int timeout_ms,
    const volatile int *running)
{
    long long deadline;
    long long start;
    int last_error;

    /*
     * 先切非阻塞再 connect：
     *   阻塞 socket 上的 connect 无法被 poll 提前打断，
     *   非阻塞 connect 会立刻返回 EINPROGRESS，由 poll 负责等待。
     */
    if(tcp_client_set_nonblocking(socket_fd) != 0)
    {
        return -1;
    }

    if(connect(
            socket_fd,
            (const struct sockaddr *)address,
            sizeof(*address)) == 0)
    {
        /*
         * 环回地址等场景可能立即成功。
         * 仍需恢复阻塞模式，保持与旧接口一致的收发语义。
         */
        if(tcp_client_set_blocking(socket_fd) != 0)
        {
            return -1;
        }

        return 0;
    }

    if(errno != EINPROGRESS)
    {
        /*
         * EAGAIN 等立即失败的情况直接上报；
         * EINTR 意味着 connect 被信号打断，连接状态未知，
         * 也按失败处理，由上层重试。
         */
        return -1;
    }

    start = tcp_client_monotonic_ms();

    if(start < 0)
    {
        errno = EIO;
        return -1;
    }

    deadline = start + timeout_ms;

    for(;;)
    {
        struct pollfd pfd;
        int poll_result;
        long long remaining;

        if(running != NULL && !(*running))
        {
            errno = EINTR;
            return -1;
        }

        remaining = deadline - tcp_client_monotonic_ms();

        if(remaining <= 0)
        {
            errno = ETIMEDOUT;
            return -1;
        }

        if(remaining > TCP_CLIENT_CONNECT_POLL_SLICE_MS)
        {
            remaining = TCP_CLIENT_CONNECT_POLL_SLICE_MS;
        }

        pfd.fd = socket_fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        poll_result = poll(&pfd, 1, (int)remaining);

        if(poll_result < 0)
        {
            /*
             * EINTR 只表示本次等待被信号打断，
             * 不重置 deadline，避免反复获得完整超时。
             */
            if(errno == EINTR)
            {
                continue;
            }

            return -1;
        }

        if(poll_result == 0)
        {
            continue;
        }

        /*
         * POLLOUT / POLLERR / POLLHUP 都表示"结果已就绪"，
         * 真正的连接结果必须用 SO_ERROR 读取。
         * 其余事件（如 POLLNVAL）按失败处理。
         */
        if((pfd.revents &
            (POLLOUT | POLLERR | POLLHUP | POLLNVAL)) == 0)
        {
            continue;
        }

        {
            int so_error = 0;
            socklen_t error_len = sizeof(so_error);

            if(getsockopt(
                    socket_fd,
                    SOL_SOCKET,
                    SO_ERROR,
                    &so_error,
                    &error_len) != 0)
            {
                return -1;
            }

            if(so_error != 0)
            {
                errno = so_error;
                return -1;
            }
        }

        /*
         * 连接已建立：恢复阻塞模式。
         *
         * 调用方 server_link_worker 之后还会显式再调一次
         * tcp_client_set_nonblocking()，那是后续收发的需要，
         * 与这里"返回时保证是同一种可用 socket"不冲突。
         */
        if(tcp_client_set_blocking(socket_fd) != 0)
        {
            return -1;
        }

        return 0;
    }

    /* 不可达，保留以消除部分编译器的"缺少返回值"告警 */
    last_error = ETIMEDOUT;
    errno = last_error;
    return -1;
}

int tcp_client_connect(const char *server_ip,uint16_t port)
{
    return tcp_client_connect_stoppable(
        server_ip, port,
        TCP_CLIENT_CONNECT_TIMEOUT_MS, NULL);
}

int tcp_client_connect_stoppable(
    const char *server_ip,
    uint16_t port,
    int timeout_ms,
    const volatile int *running)
{
    int client_socket_fd = -1;
    struct sockaddr_in serveraddr;

    if(server_ip==NULL||server_ip[0]=='\0')
    {
        errno =EINVAL;
        return -1;
    }

    if(timeout_ms <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    client_socket_fd = socket(AF_INET,SOCK_STREAM,0);
    if(client_socket_fd<0)
    {
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

    if(tcp_client_connect_bounded(
            client_socket_fd,
            &serveraddr,
            timeout_ms,
            running) != 0)
    {
        /*
         * 所有失败分支统一在这里关闭 socket。
         * 先保存 errno，避免 close 覆盖真实错误码。
         */
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