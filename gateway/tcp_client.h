#ifndef TCP_CLIENT_H
#define TCP_CLIENT_H

#include <stddef.h>
#include <stdint.h>

/*
 * 连接TCP服务器。
 * 成功返回socket文件描述符，失败返回-1。
 */
int tcp_client_connect(
    const char *server_ip,
    uint16_t port
);

/*
 * 保证把length字节全部发送出去。
 *
 * 内部用 poll(POLLOUT) 带超时等待，
 * 对端长时间不读时会在超时后返回 -1（errno=ETIMEDOUT），
 * 而不会无限阻塞。
 *
 * 配合 tcp_client_set_nonblocking() 使用时行为最完整；
 * 即使是阻塞 socket 也能因 poll 超时而返回。
 *
 * 成功返回0，失败返回-1。
 */
int tcp_client_send_all(
    int socket_fd,
    const void *data,
    size_t length
);

/*
 * 把已连接 socket 置为非阻塞。
 *
 * 建议在 connect 成功后、开始收发前调用，
 * 使 recv/send 都不会意外阻塞。
 *
 * @return 0成功；-1失败（errno 已设置）
 */
int tcp_client_set_nonblocking(int socket_fd);

/*
 * 关闭TCP连接。
 */
void tcp_client_close(int socket_fd);

#endif