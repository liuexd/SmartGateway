/*
 * 让系统声明cfmakeraw()等接口。
 * 必须放在所有头文件前面。
 */
#define _DEFAULT_SOURCE

#include "serial_port.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <termios.h>
#include <unistd.h>

/*
 * 将普通整数波特率转换为termios使用的常量。
 */
static speed_t baud_rate_to_speed(int baud_rate)
{
    switch (baud_rate)
    {
        case 1200:
            return B1200;

        case 2400:
            return B2400;

        case 4800:
            return B4800;

        case 9600:
            return B9600;

        case 19200:
            return B19200;

        case 38400:
            return B38400;

        case 57600:
            return B57600;

        case 115200:
            return B115200;

        default:
            return (speed_t)0;
    }
}

/*
 * 某个系统调用失败后，需要关闭fd，
 * 但又不能让close()覆盖原来的errno。
 */
static void close_preserve_errno(int fd)
{
    int saved_errno = errno;

    close(fd);

    errno = saved_errno;
}

int serial_port_open(
    const char *device,
    int baud_rate
)
{
    struct termios options;
    speed_t speed;
    int flags;
    int fd;

    if (device == NULL || device[0] == '\0')
    {
        errno = EINVAL;
        return -1;
    }

    speed = baud_rate_to_speed(baud_rate);

    if (speed == (speed_t)0)
    {
        /*
         * 传入了当前程序不支持的波特率。
         */
        errno = EINVAL;
        return -1;
    }

    /*
     * O_RDWR：
     * 串口需要同时进行读写。
     *
     * O_NOCTTY：
     * 不把串口设置为当前程序的控制终端。
     *
     * O_NONBLOCK：
     * 非阻塞打开，后续使用poll等待事件。
     */
    flags = O_RDWR | O_NOCTTY | O_NONBLOCK;

#ifdef O_CLOEXEC
    /*
     * 将来程序调用exec时，不把串口fd传给新程序。
     */
    flags |= O_CLOEXEC;
#endif

    fd = open(device, flags);

    if (fd < 0)
    {
        return -1;
    }

    /*
     * 读取当前串口配置。
     */
    if (tcgetattr(fd, &options) != 0)
    {
        close_preserve_errno(fd);
        return -1;
    }

    /*
     * 设置raw模式。
     *
     * raw模式下，Linux不会自动修改数据：
     * 不处理回车换行；
     * 不回显；
     * 不进行行缓冲；
     * 每个字节原样交给程序。
     */
    cfmakeraw(&options);

    /*
     * 清除原数据位设置，再设置为8位数据位。
     */
    options.c_cflag &= (tcflag_t)~CSIZE;
    options.c_cflag |= CS8;

    /*
     * CLOCAL：
     * 忽略调制解调器控制线。
     *
     * CREAD：
     * 启用接收功能。
     */
    options.c_cflag |= CLOCAL | CREAD;

    /*
     * 无校验。
     */
    options.c_cflag &= (tcflag_t)~PARENB;

    /*
     * 1个停止位。
     */
    options.c_cflag &= (tcflag_t)~CSTOPB;

    /*
     * 关闭硬件流控。
     */
#ifdef CRTSCTS
    options.c_cflag &= (tcflag_t)~CRTSCTS;
#endif

    /*
     * 关闭软件流控。
     */
    options.c_iflag &= (tcflag_t)~(IXON | IXOFF | IXANY);

    /*
     * 因为fd处于非阻塞状态，
     * 实际等待由poll()负责。
     */
    options.c_cc[VMIN] = 0;
    options.c_cc[VTIME] = 0;

    /*
     * 配置输入与输出波特率。
     */
    if (cfsetispeed(&options, speed) != 0 ||
        cfsetospeed(&options, speed) != 0)
    {
        close_preserve_errno(fd);
        return -1;
    }

    /*
     * 立即应用串口配置。
     */
    if (tcsetattr(fd, TCSANOW, &options) != 0)
    {
        close_preserve_errno(fd);
        return -1;
    }

    /*
     * 清除打开前残留的输入和输出数据。
     */
    if (tcflush(fd, TCIOFLUSH) != 0)
    {
        close_preserve_errno(fd);
        return -1;
    }

    return fd;
}

int serial_port_wait_readable(
    int fd,
    int timeout_ms
)
{
    struct pollfd pfd;
    int result;

    if (fd < 0 || timeout_ms < -1)
    {
        errno = EINVAL;
        return SERIAL_WAIT_ERROR;
    }

    pfd.fd = fd;

    /*
     * POLLIN表示等待数据可读。
     *
     * POLLERR、POLLHUP、POLLNVAL即使不写入events，
     * poll也会通过revents返回。
     */
    pfd.events = POLLIN;
    pfd.revents = 0;

    result = poll(&pfd, 1, timeout_ms);

    /*
     * 信号中断poll时，不认为是串口错误。
     * 返回超时，让main重新检查退出标志。
     */
    if (result < 0 && errno == EINTR)
    {
        return SERIAL_WAIT_TIMEOUT;
    }

    if (result < 0)
    {
        return SERIAL_WAIT_ERROR;
    }

    if (result == 0)
    {
        return SERIAL_WAIT_TIMEOUT;
    }

    /*
     * fd已经无效。
     */
    if ((pfd.revents & POLLNVAL) != 0)
    {
        errno = EBADF;
        return SERIAL_WAIT_ERROR;
    }

    /*
     * 串口发生I/O错误。
     */
    if ((pfd.revents & POLLERR) != 0)
    {
        errno = EIO;
        return SERIAL_WAIT_ERROR;
    }

    /*
     * 如果既有数据又发生挂断，先读取剩余数据。
     * 下一次poll再处理POLLHUP。
     */
    if ((pfd.revents & POLLIN) != 0)
    {
        return SERIAL_WAIT_READABLE;
    }

    /*
     * 对端关闭或设备断开。
     */
    if ((pfd.revents & POLLHUP) != 0)
    {
        return SERIAL_WAIT_HANGUP;
    }

    return SERIAL_WAIT_TIMEOUT;
}

ssize_t serial_port_read(
    int fd,
    void *buffer,
    size_t size
)
{
    ssize_t result;

    if (fd < 0 || buffer == NULL || size == 0U)
    {
        errno = EINVAL;
        return -1;
    }

    /*
     * 如果read被普通信号中断，则重新读取。
     */
    do
    {
        result = read(fd, buffer, size);
    }
    while (result < 0 && errno == EINTR);

    /*
     * 非阻塞串口目前暂时没有数据。
     * 这不属于真正的I/O错误。
     */
    if (result < 0 &&
        (errno == EAGAIN || errno == EWOULDBLOCK))
    {
        return SERIAL_READ_WOULD_BLOCK;
    }

    return result;
}

int serial_port_write_all(
    int fd,
    const void *buffer,
    size_t length,
    int timeout_ms
)
{
    const uint8_t *bytes;
    size_t total_written = 0;

    if (fd < 0 ||
        (buffer == NULL && length > 0U) ||
        timeout_ms < -1)
    {
        errno = EINVAL;
        return -1;
    }

    bytes = (const uint8_t *)buffer;

    /*
     * write()可能一次只写入部分数据，
     * 所以必须不断循环，直到全部写完。
     */
    while (total_written < length)
    {
        struct pollfd pfd;
        int poll_result;
        ssize_t write_result;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;

        /*
         * 等待串口可以写入。
         */
        do
        {
            poll_result = poll(
                &pfd,
                1,
                timeout_ms
            );
        }
        while (poll_result < 0 && errno == EINTR);

        if (poll_result < 0)
        {
            return -1;
        }

        if (poll_result == 0)
        {
            errno = ETIMEDOUT;
            return -1;
        }

        if ((pfd.revents &
             (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            errno = EIO;
            return -1;
        }

        if ((pfd.revents & POLLOUT) == 0)
        {
            continue;
        }

        write_result = write(
            fd,
            bytes + total_written,
            length - total_written
        );

        if (write_result > 0)
        {
            total_written += (size_t)write_result;
            continue;
        }

        if (write_result < 0 && errno == EINTR)
        {
            continue;
        }

        if (write_result < 0 &&
            (errno == EAGAIN ||
             errno == EWOULDBLOCK))
        {
            continue;
        }

        /*
         * write返回0通常表示设备出现异常。
         */
        if (write_result == 0)
        {
            errno = EIO;
        }

        return -1;
    }

    return 0;
}

void serial_port_close(int fd)
{
    if (fd >= 0)
    {
        close(fd);
    }
}