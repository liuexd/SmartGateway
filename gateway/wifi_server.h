#ifndef WIFI_SERVER_H
#define WIFI_SERVER_H

#include <stdint.h>

/*
 * 创建 WiFi 节点监听 socket。
 *
 * 成功返回 listen_fd。
 * 失败返回 -1。
 */

int wifi_server_open(uint16_t port);

/*
 * 接收一个新的 WiFi 节点连接。
 *
 * 成功返回 client_fd。
 * 失败返回 -1。
 */

int wifi_server_accept(int listen_fd);

/*
 * 关闭 socket。
 */

void wifi_server_close(int fd);



#endif // WIFI_SERVER_H