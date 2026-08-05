#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <stddef.h>
#include <sys/types.h>

/*
 * serial_port_wait_readable()的返回值。
 */
#define SERIAL_WAIT_ERROR     (-1)
#define SERIAL_WAIT_HANGUP    (-2)
#define SERIAL_WAIT_TIMEOUT   0
#define SERIAL_WAIT_READABLE  1

/*
 * serial_port_read()返回该值，表示当前暂时没有数据，
 * 并不代表串口发生错误。
 */
#define SERIAL_READ_WOULD_BLOCK ((ssize_t)-2)

/**
 * @brief 打开并配置串口。
 *
 * 配置内容：
 * 8个数据位
 * 1个停止位
 * 无校验
 * 无软硬件流控
 * raw模式
 * 非阻塞模式
 *
 * @param device    设备路径，例如/tmp/ttyGW或/dev/rfcomm0
 * @param baud_rate 波特率，例如9600
 *
 * @return 成功返回文件描述符，失败返回-1。
 */
int serial_port_open(
    const char *device,
    int baud_rate
);

/**
 * @brief 使用poll等待串口可读或断开。
 *
 * @param fd         串口文件描述符
 * @param timeout_ms 超时时间，毫秒；-1表示永久等待
 *
 * @return SERIAL_WAIT_READABLE：有数据
 *         SERIAL_WAIT_TIMEOUT：超时
 *         SERIAL_WAIT_HANGUP：设备断开
 *         SERIAL_WAIT_ERROR：错误
 */
int serial_port_wait_readable(
    int fd,
    int timeout_ms
);

/**
 * @brief 从串口读取数据。
 *
 * @return 大于0：实际读取字节数
 *         0：设备EOF或断开
 *         SERIAL_READ_WOULD_BLOCK：暂时没有数据
 *         -1：读取错误
 */
ssize_t serial_port_read(
    int fd,
    void *buffer,
    size_t size
);

/**
 * @brief 保证把全部数据写入串口。
 *
 * write()可能只写一部分，所以函数内部会循环写入。
 *
 * @return 成功返回0，失败返回-1。
 */
int serial_port_write_all(
    int fd,
    const void *buffer,
    size_t length,
    int timeout_ms
);

/**
 * @brief 关闭串口。
 */
void serial_port_close(int fd);

#endif