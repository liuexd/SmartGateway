#define _POSIX_C_SOURCE 200809L

#include "tcp_server.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/*
 * 监听队列长度。
 */
#define TCP_SERVER_LISTEN_BACKLOG 5

/*
 * 创建监听socket。
 */
int tcp_server_create(unsigned int port)
{
    struct sockaddr_in server_addr;
    int server_fd;
    int reuseaddr = 1;

    server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0)
    {
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons((uint16_t)port);

    /*
     * 只允许本地127.0.0.1连接。
     */
    server_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    /*
     * 允许端口快速重用，避免重启时TIME_WAIT阻塞。
     */
    if (setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &reuseaddr,
            sizeof(reuseaddr)
        ) != 0)
    {
        int save_error = errno;
        close(server_fd);
        errno = save_error;
        return -1;
    }

    if (bind(
            server_fd,
            (const struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) != 0)
    {
        int save_error = errno;
        close(server_fd);
        errno = save_error;
        return -1;
    }

    if (listen(server_fd, TCP_SERVER_LISTEN_BACKLOG) != 0)
    {
        int save_error = errno;
        close(server_fd);
        errno = save_error;
        return -1;
    }

    return server_fd;
}

/*
 * 接受一个客户端连接。
 */
int tcp_server_accept(
    int server_fd,
    char *client_ip,
    size_t client_ip_size,
    unsigned short *client_port
)
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len;
    int client_fd;

    if (server_fd < 0 ||
        client_ip == NULL ||
        client_ip_size == 0U ||
        client_port == NULL)
    {
        return -1;
    }

    client_addr_len = sizeof(client_addr);

    client_fd = accept(
        server_fd,
        (struct sockaddr *)&client_addr,
        &client_addr_len
    );

    if (client_fd < 0)
    {
        return -1;
    }

    if (inet_ntop(
            AF_INET,
            &client_addr.sin_addr,
            client_ip,
            (socklen_t)client_ip_size
        ) == NULL)
    {
        strncpy(client_ip, "unknown", client_ip_size);
        client_ip[client_ip_size - 1U] = '\0';
    }

    *client_port = ntohs(client_addr.sin_port);

    return client_fd;
}

/*
 * 关闭socket。
 */
void tcp_server_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}
