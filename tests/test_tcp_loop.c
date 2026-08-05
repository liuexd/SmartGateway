#include "tcp_client.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>


int main(void)
{
    int socket_fd;
    unsigned int sequence = 0;

    socket_fd = tcp_client_connect(
        "127.0.0.1",
        9000
    );

    if (socket_fd < 0)
    {
        perror("tcp_client_connect");
        return 1;
    }

    printf("Connected to server.\n");

    while (1)
    {
        char message[256];

        int length = snprintf(
            message,
            sizeof(message),
            "{\"node\":\"NODE01\","
            "\"seq\":%u,"
            "\"type\":\"DATA\","
            "\"temperature\":25.3,"
            "\"humidity\":60.1}\n",
            sequence
        );

        if (length < 0 ||
            (size_t)length >= sizeof(message))
        {
            fprintf(
                stderr,
                "snprintf failed\n"
            );

            break;
        }

        if (tcp_client_send_all(
                socket_fd,
                message,
                (size_t)length
            ) != 0)
        {
            perror("tcp_client_send_all");
            break;
        }

        printf(
            "Sent sequence: %u\n",
            sequence
        );

        sequence++;

        sleep(1);
    }

    tcp_client_close(socket_fd);

    printf("TCP client stopped.\n");

    return 0;
}