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
 * 成功返回0，失败返回-1。
 */
int tcp_client_send_all(
    int socket_fd,
    const void *data,
    size_t length
);

/*
 * 关闭TCP连接。
 */
void tcp_client_close(int socket_fd);

#endif