
#ifndef OTA_HEALTH_H
#define OTA_HEALTH_H

#include <sys/types.h>

/*
 * 等待 Gateway 子进程的 TCP 服务就绪。
 *
 * gateway_pid 必须是调用进程创建且尚未回收的子进程。
 * ipv4_address 例如 "127.0.0.1"。
 *
 * 返回 0：子进程存活，且 TCP 连接成功。
 * 返回 -1：超时、子进程退出或系统调用失败。
 *
 * 注意：这只是 L1 就绪检测，不代表业务健康。
 */
int ota_health_wait_tcp_ready(
    pid_t gateway_pid,
    const char *ipv4_address,
    unsigned short port,
    int timeout_ms
);

#endif
