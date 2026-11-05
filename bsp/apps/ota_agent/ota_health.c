
#define _GNU_SOURCE

#include "ota_health.h"

#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    return (long long)ts.tv_sec * 1000LL +
           ts.tv_nsec / 1000000LL;
}

/*
 * 非破坏性检查子进程是否已退出。
 * WNOWAIT：保留退出状态，供 ota_process_stop()
 * 之后使用 waitpid() 正式回收。
 */
static int child_alive(pid_t pid)
{
    siginfo_t info;

    memset(&info, 0, sizeof(info));

    if (waitid(
            P_PID,
            (id_t)pid,
            &info,
            WEXITED | WNOHANG | WNOWAIT) != 0)
        return -1;

    return info.si_pid == 0 ? 1 : 0;
}

static int tcp_probe(
    const struct sockaddr_in *address)
{
    int fd;
    int ret;
    int error = 0;
    socklen_t error_len = sizeof(error);

    fd = socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0);

    if (fd < 0)
        return -1;

    ret = connect(
        fd,
        (const struct sockaddr *)address,
        sizeof(*address));

    if (ret == 0) {
        close(fd);
        return 0;
    }

    if (errno != EINPROGRESS) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    struct pollfd pfd = {
        .fd = fd,
        .events = POLLOUT
    };

    do {
        ret = poll(&pfd, 1, 100);
    } while (ret < 0 && errno == EINTR);

    if (ret <= 0) {
        int saved = ret == 0 ? ETIMEDOUT : errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (getsockopt(
            fd, SOL_SOCKET, SO_ERROR,
            &error, &error_len) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    close(fd);

    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

int ota_health_wait_tcp_ready(
    pid_t gateway_pid,
    const char *ipv4_address,
    unsigned short port,
    int timeout_ms)
{
    struct sockaddr_in address;
    long long start;

    if (gateway_pid <= 1 ||
        ipv4_address == NULL ||
        port == 0 ||
        timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(
            AF_INET,
            ipv4_address,
            &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    start = now_ms();

    if (start < 0)
        return -1;

    for (;;) {
        int alive = child_alive(gateway_pid);

        if (alive <= 0) {
            if (alive == 0)
                errno = ECHILD;
            return -1;
        }

        if (tcp_probe(&address) == 0) {
            /*
             * TCP 成功后再检查一次，防止 Gateway
             * 在连接成功后立即退出。
             */
            alive = child_alive(gateway_pid);

            if (alive == 1)
                return 0;

            if (alive == 0)
                errno = ECHILD;
            return -1;
        }

        long long current = now_ms();

        if (current < 0)
            return -1;

        if (current - start >= timeout_ms) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 50 * 1000 * 1000
        };

        while (nanosleep(&delay, &delay) != 0 &&
               errno == EINTR) {
        }
    }
}
