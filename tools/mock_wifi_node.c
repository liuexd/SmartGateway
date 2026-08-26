#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#include <signal.h>

#include "message_json.h"

static volatile sig_atomic_t g_running = 1;

static void handle_stop_signal(int signo)
{
    (void)signo;

    g_running = 0;
}

static int install_signal_handlers(void)
{
    struct sigaction action;
    struct sigaction pipe_action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = handle_stop_signal;

    sigemptyset(&action.sa_mask);

    action.sa_flags = 0;
    if(sigaction(SIGINT, &action, NULL)<0)
    {
        return -1;
    }

    if(sigaction(SIGTERM, &action, NULL)<0)
    {
        return -1;
    }

    memset(&pipe_action, 0, sizeof(pipe_action));
    
    pipe_action.sa_handler = SIG_IGN;

    sigemptyset(&pipe_action.sa_mask);

    if(sigaction(SIGPIPE, &pipe_action, NULL) < 0)
    {
        return -1;
    }
    return 0;
}
/*
 * 将端口号文本转换成整数（1~65535）。
 */
static int parse_port(
    const char *text,
    unsigned short *port
)
{
    char *end = NULL;
    long value;

    if (text == NULL ||
        port == NULL ||
        text[0] == '\0')
    {
        return -1;
    }

    errno = 0;

    value = strtol(text, &end, 10);

    if (errno != 0 ||
        end == text ||
        *end != '\0' ||
        value < 1 ||
        value > 65535)
    {
        return -1;
    }

    *port = (unsigned short)value;

    return 0;
}

/*
 *
 */
static int send_all(
    int fd,
    const char *buffer,
    size_t length
)
{
    size_t total = 0;
    while(total < length)
    {
        ssize_t sent;

        sent = send (
            fd,
            buffer + total,
            length - total,
            0
        );

        if(sent < 0 )
        {
            if(errno == EINTR)
            {
                if(!g_running)
                {
                    return -1;
                }
                continue;
            }

        return -1;
        }

        if(sent == 0)
        {
            errno = EPIPE;
            return -1;
        }

        total += (size_t)sent;
    }
    return 0;
}

int main(int argc, char *argv[])
{
    const char *server_ip;
    const char *node_id;
    unsigned short port;
    int sockfd;
    struct sockaddr_in server_addr;

    /*
     * 输入参数解析。
     */
    if (argc != 4)
    {
        fprintf(
            stderr,
            "Usage: %s <server_ip> <port> <node_id>\n",
            argv[0]
        );

        return 1;
    }

    server_ip = argv[1];
    node_id = argv[3];

    if (parse_port(argv[2], &port) != 0)
    {
        fprintf(
            stderr,
            "Invalid port: %s (must be 1-65535)\n",
            argv[2]
        );

        return 1;
    }

    printf(
        "server=%s port=%u node=%s\n",
        server_ip,
        (unsigned int)port,
        node_id
    );

    /*
     * TCP连接。
     */

    sockfd = socket(AF_INET, SOCK_STREAM, 0);

    if (sockfd < 0)
    {
        perror("socket");
        return 1;
    }

    memset(&server_addr, 0, sizeof(server_addr));

    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(port);

    if (inet_pton(
        AF_INET,
        server_ip,
        &server_addr.sin_addr
    ) != 1)
    {
        fprintf(stderr, "Invalid IPv4 address: %s\n", server_ip);
        close(sockfd);
        return 1;
    }
    printf(
        "Connecting to %s:%u...\n",
        server_ip,
        (unsigned int)port
    );

    if (connect(
            sockfd,
            (struct sockaddr *)&server_addr,
            sizeof(server_addr)
        ) < 0)
    {
        perror("connect");
        close(sockfd);
        return 1;
    }
    printf("Connected successfully\n");

    uint32_t sequence = 1;

    if(install_signal_handlers() !=0)
    {
        perror("sigaction");
        close(sockfd);
        return 1;
    }

    while(g_running)
    {
        frame_data_t data;
        char json[256];
        int json_length;
        int light_raw;

        memset(&data,0,sizeof(data));

        light_raw = 2000 + (int) (sequence % 1000U);

        snprintf(
            data.node_id,
            sizeof(data.node_id),
            "%s",
            node_id
        );

        data.sequence = sequence;

        snprintf(
            data.fields[0].key,
            sizeof(data.fields[0].key),
            "%s",
            "LIGHT_RAW"
        );

        snprintf(
            data.fields[0].value,
            sizeof(data.fields[0].value),
            "%d",
           light_raw
    );
        data.field_count = 1;

        json_length = message_json_build_data(
            json,
            sizeof(json),
            &data
        );    
        if(json_length < 0 )
        {
            fprintf(stderr, "Failed to build JSON\n");
            close(sockfd);
            return -1;
        }

        if(send_all(
            sockfd,
            json,
            (size_t)json_length
        ) != 0)
        {
            if(!g_running)
            {
                break;
            }

        perror("send");
        break;
        }

        printf("TX : %s\n",json);

        sequence++;

        sleep(2);
    }
    printf("Closing connection ...\n");

    close(sockfd);

    printf("mock_wifi_node stop\n");

    return 0;
}