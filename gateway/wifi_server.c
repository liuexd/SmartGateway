/* 模块 wifi_server：创建监听 socket 并接受 WiFi 连接；客户端数据由 worker 处理。 */
#include "wifi_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

int wifi_server_open(uint16_t port)
{
    int listen_fd;
    int reuse = 1;
    struct sockaddr_in address;

    /* 创建 IPv4 TCP socket。 */

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0)
    {
        return -1;
    }

    if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        close(listen_fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    /* 将 socket 绑定到监听端口。 */
    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        close(listen_fd);
        return -1;
    }

    /* 开始监听连接。 */
    if (listen(listen_fd, 8) < 0)
    {
        close(listen_fd);
        return -1;
    }

    return listen_fd;
}

int wifi_server_accept(int listen_fd)
{
    int client_fd;

    client_fd = accept(listen_fd, NULL, NULL);

    if (client_fd < 0)
    {
        return -1;
    }

    return client_fd;
}

void wifi_server_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}
