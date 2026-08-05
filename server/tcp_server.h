#ifndef TCP_SERVER_H
#define TCP_SERVER_H

#include <stddef.h>
#include <stdint.h>

/*
 * =====================================================================
 * TCP服务器（监听/连接管理）
 *
 * 职责：
 *   1. 创建监听socket（绑定127.0.0.1）；
 *   2. 接受客户端连接；
 *   3. 关闭连接。
 *
 * 消息解析（按行拆分）见 common/line_parser.h。
 * =====================================================================
 */

/*
 * 创建监听socket并开始监听。
 *
 * 只绑定本地回环地址 127.0.0.1。
 *
 * @param port 监听端口
 *
 * @return 成功返回监听socket描述符，失败返回-1
 */
int tcp_server_create(unsigned int port);

/*
 * 接受一个客户端连接。
 *
 * @param server_fd   监听socket
 * @param client_ip   输出：客户端IP字符串（至少INET_ADDRSTRLEN字节）
 * @param client_ip_size client_ip缓冲区大小
 * @param client_port 输出：客户端端口
 *
 * @return 成功返回客户端socket描述符；失败返回-1
 *         （被信号打断时返回-1且errno==EINTR，可重试）
 */
int tcp_server_accept(
    int server_fd,
    char *client_ip,
    size_t client_ip_size,
    unsigned short *client_port
);

/*
 * 关闭socket（不修改errno）。
 */
void tcp_server_close(int fd);

#endif
