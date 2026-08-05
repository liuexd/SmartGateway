#define _DEFAULT_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "tcp_client.h"

#include <stdio.h>
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

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
        //MSG_NOSIGNAL防止产生SGIPIPE信号会导致进程被杀
        ssize_t retsize = send(socket_fd,bytes+total_send,length - total_send,
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