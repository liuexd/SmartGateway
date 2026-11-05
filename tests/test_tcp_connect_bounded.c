/*
 * test_tcp_connect_bounded.c - tcp_client 有界 connect 的行为验证
 *
 * 用法：
 *   test_tcp_connect_bounded success IP PORT
 *   test_tcp_connect_bounded refused IP PORT
 *   test_tcp_connect_bounded bounded IP PORT TIMEOUT_MS
 *   test_tcp_connect_bounded cancel  IP PORT TIMEOUT_MS
 *   test_tcp_connect_bounded fdleak  IP PORT
 *
 * 输出保持单行 key=value，便于 Python 侧断言。
 */

#define _POSIX_C_SOURCE 200809L

#include "tcp_client.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile int g_running = 1;

static long long now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (long long)ts.tv_sec * 1000LL +
           ts.tv_nsec / 1000000LL;
}

static int is_blocking(int fd)
{
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0)
        return -1;

    return (flags & O_NONBLOCK) == 0 ? 1 : 0;
}

static int count_open_fds(void)
{
    /* 用 /proc/self/fd 统计，避免依赖 rlimit 猜测 */
    int count = 0;

    for (int fd = 0; fd < 1024; fd++)
    {
        if (fcntl(fd, F_GETFD) >= 0)
            count++;
    }

    return count;
}

/* 取消线程：稍等片刻后把停止标志置 0 */
static void *cancel_thread(void *arg)
{
    (void)arg;

    struct timespec delay = {
        .tv_sec = 0,
        .tv_nsec = 200 * 1000 * 1000
    };

    nanosleep(&delay, NULL);

    g_running = 0;

    return NULL;
}

static int cmd_success(const char *ip, unsigned short port)
{
    int fd = tcp_client_connect(ip, port);

    if (fd < 0)
    {
        printf("rc=%d errno=%d\n", -1, errno);
        return 1;
    }

    printf("rc=0 fd_ok=1 blocking=%d\n", is_blocking(fd));

    tcp_client_close(fd);
    return 0;
}

static int cmd_refused(const char *ip, unsigned short port)
{
    int saved;

    errno = 0;

    if (tcp_client_connect(ip, port) >= 0)
    {
        printf("rc=0 unexpected\n");
        return 1;
    }

    saved = errno;

    printf("rc=-1 errno=%d\n", saved);
    return saved == ECONNREFUSED ? 0 : 1;
}

static int cmd_bounded(
    const char *ip, unsigned short port, int timeout_ms)
{
    long long start = now_ms();
    int saved;

    errno = 0;

    if (tcp_client_connect_stoppable(
            ip, port, timeout_ms, NULL) >= 0)
    {
        printf("rc=0 unexpected\n");
        return 1;
    }

    saved = errno;

    printf(
        "rc=-1 errno=%d elapsed_ms=%lld\n",
        saved, now_ms() - start);

    return saved == ETIMEDOUT ? 0 : 1;
}

static int cmd_cancel(
    const char *ip, unsigned short port, int timeout_ms)
{
    pthread_t tid;
    long long start;
    int ret;
    int saved;

    g_running = 1;

    if (pthread_create(&tid, NULL, cancel_thread, NULL) != 0)
        return 1;

    start = now_ms();

    errno = 0;

    ret = tcp_client_connect_stoppable(
        ip, port, timeout_ms, &g_running);

    saved = errno;

    pthread_join(tid, NULL);

    if (ret >= 0)
    {
        printf("rc=0 unexpected\n");
        tcp_client_close(ret);
        return 1;
    }

    printf(
        "rc=-1 errno=%d elapsed_ms=%lld\n",
        saved, now_ms() - start);

    return saved == EINTR ? 0 : 1;
}

static int cmd_fdleak(const char *ip, unsigned short port)
{
    int before = count_open_fds();

    /* 连续失败多次，确认每次都完成了关闭 */
    for (int i = 0; i < 64; i++)
    {
        (void)tcp_client_connect_stoppable(
            ip, port, 500, NULL);
    }

    int after = count_open_fds();

    printf(
        "before=%d after=%d leak=%d\n",
        before, after, after - before);

    return after == before ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: %s MODE IP PORT [TIMEOUT_MS]\n", argv[0]);
        return 2;
    }

    const char *mode = argv[1];
    const char *ip = argv[2];
    unsigned short port = (unsigned short)atoi(argv[3]);

    if (strcmp(mode, "success") == 0)
        return cmd_success(ip, port);

    if (strcmp(mode, "refused") == 0)
        return cmd_refused(ip, port);

    if (strcmp(mode, "fdleak") == 0)
        return cmd_fdleak(ip, port);

    if (argc < 5)
    {
        fprintf(stderr, "%s needs TIMEOUT_MS\n", mode);
        return 2;
    }

    int timeout_ms = atoi(argv[4]);

    if (strcmp(mode, "bounded") == 0)
        return cmd_bounded(ip, port, timeout_ms);

    if (strcmp(mode, "cancel") == 0)
        return cmd_cancel(ip, port, timeout_ms);

    fprintf(stderr, "unknown mode: %s\n", mode);
    return 2;
}
