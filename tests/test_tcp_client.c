#include "tcp_client.h"

#include <stdio.h>
#include <string.h>


int main(void)
{
    const char *server_ip = "127.0.0.1";
    unsigned short server_port = 9000;

    int socket_fd;

    printf(
        "Connecting to %s:%u...\n",
        server_ip,
        (unsigned int)server_port
    );

    socket_fd = tcp_client_connect(
        server_ip,
        server_port
    );

    if (socket_fd < 0)
    {
        perror("tcp_client_connect");
        return 1;
    }

    printf("Connected.\n");

    /*
     * 注意最后必须包含\n。
     *
     * 因为M4-1服务端使用\n作为消息边界。
     */
    const char message1[] =
        "{\"node\":\"NODE01\","
        "\"seq\":1,"
        "\"type\":\"DATA\","
        "\"temperature\":25.3,"
        "\"humidity\":60.1}\n";

    const char message2[] =
        "{\"node\":\"NODE01\","
        "\"seq\":2,"
        "\"type\":\"DATA\","
        "\"temperature\":25.4,"
        "\"humidity\":60.2}\n";

    /*
     * 第一条消息。
     */
    if (tcp_client_send_all(
            socket_fd,
            message1,
            strlen(message1)
        ) != 0)
    {
        perror("send message1");

        tcp_client_close(socket_fd);
        return 1;
    }

    printf("Message 1 sent.\n");

    /*
     * 不关闭TCP连接。
     *
     * 继续在同一个连接发送第二条消息。
     */
    if (tcp_client_send_all(
            socket_fd,
            message2,
            strlen(message2)
        ) != 0)
    {
        perror("send message2");

        tcp_client_close(socket_fd);
        return 1;
    }

    printf("Message 2 sent.\n");

    tcp_client_close(socket_fd);

    printf("Connection closed.\n");

    return 0;
}