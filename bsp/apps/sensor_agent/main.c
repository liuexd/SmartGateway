
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <signal.h>
#include <time.h>
#include <poll.h>

#define GATEWAY_IP   "127.0.0.1"
#define GATEWAY_PORT 7000
#define SAMPLE_PERIOD_MS  2000
#define CONNECT_TIMEOUT_MS 2000
#define SEND_TIMEOUT_MS  1000
#define RETRY_INTERVAL_MS 2000
#define HEARTBEAT_PERIOD_MS 10000

#define ALS_PATH \
    "/sys/bus/i2c/devices/0-001e/als_raw"

static volatile sig_atomic_t g_running = 1;

static void handle_stop(int signo)
{
    (void)signo;
    g_running = 0;
}

static int install_signals(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = handle_stop;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGINT, &sa, NULL) != 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) != 0)
        return -1;
    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int close_with_errno(int fd)
{
    int saved = errno;
    close(fd);
    errno = saved;
    return -1;
}

/* TCP 连接最长等待 2 秒，socket 保持 O_NONBLOCK。 */
static int connect_gateway(void)
{
    struct sockaddr_in addr;
    struct pollfd pfd;
    int fd, flags, ret, so_error;
    socklen_t optlen = sizeof(so_error);

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
        return close_with_errno(fd);

    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(GATEWAY_PORT);
    if (inet_pton(AF_INET, GATEWAY_IP, &addr.sin_addr) != 1) {
        errno = EINVAL;
        return close_with_errno(fd);
    }

    ret = connect(fd, (struct sockaddr *)&addr, sizeof(addr));
    if (ret == 0)
        return fd;
    if (errno != EINPROGRESS && errno != EINTR)
        return close_with_errno(fd);

    pfd.fd = fd;
    pfd.events = POLLOUT;
    pfd.revents = 0;
    do {
        ret = poll(&pfd, 1, CONNECT_TIMEOUT_MS);
    } while (ret < 0 && errno == EINTR && g_running);

    if (ret == 0) {
        errno = ETIMEDOUT;
        return close_with_errno(fd);
    }
    if (ret < 0)
        return close_with_errno(fd);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen) != 0)
        return close_with_errno(fd);
    if (so_error != 0) {
        errno = so_error;
        return close_with_errno(fd);
    }
    if (!(pfd.revents & POLLOUT)) {
        errno = ECONNABORTED;
        return close_with_errno(fd);
    }
    return fd;
}

/* 一整条 JSON Lines 发送至多 1 秒；失败时由外层原样重试。 */
static int send_all_timeout(int fd, const char *data, size_t length)
{
    size_t sent = 0;
    int64_t start = monotonic_ms();
    if (start < 0)
        return -1;

    while (sent < length && g_running) {
        int64_t now = monotonic_ms();
        int remaining;
        ssize_t n;
        struct pollfd pfd;
        int ret;

        if (now < 0)
            return -1;
        remaining = SEND_TIMEOUT_MS - (int)(now - start);
        if (remaining <= 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        n = send(fd, data + sent, length - sent, MSG_NOSIGNAL);
        if (n > 0) {
            sent += (size_t)n;
            continue;
        }
        if (n == 0) {
            errno = EPIPE;
            return -1;
        }
        if (errno == EINTR)
            continue;
        if (errno != EAGAIN && errno != EWOULDBLOCK)
            return -1;

        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfd.revents = 0;
        ret = poll(&pfd, 1, remaining);
        if (ret == 0) {
            errno = ETIMEDOUT;
            return -1;
        }
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
            errno = EPIPE;
            return -1;
        }
    }
    if (!g_running) {
        errno = EINTR;
        return -1;
    }
    return 0;
}

static int build_sensor_json(
    char *out,
    size_t capacity,
    uint32_t seq,
    unsigned int als_raw)
{
    int n;

    n = snprintf(
        out,
        capacity,
        "{\"node\":\"NODE04\","
        "\"seq\":%u,"
        "\"type\":\"DATA\","
        "\"fields\":{"
        "\"DEV\":\"5\","
        "\"LIGHT_RAW\":\"%u\","
        "\"KIND\":\"SAMPLE\""
        "}}\n",
        (unsigned int)seq,
        als_raw
    );

    if (n < 0 || (size_t)n >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    return n;
}


static int build_heartbeat_json(
    char *out,
    size_t capacity,
    uint32_t seq,
    const char *sensor_status)
{
    int n;

    n = snprintf(
        out,
        capacity,
        "{\"node\":\"NODE04\","
        "\"seq\":%u,"
        "\"type\":\"DATA\","
        "\"fields\":{"
        "\"DEV\":\"5\","
        "\"KIND\":\"HEARTBEAT\","
        "\"STATUS\":\"ALIVE\","
        "\"SENSOR\":\"%s\""
        "}}\n",
        (unsigned int)seq,
        sensor_status
    );

    if (n < 0 || (size_t)n >= capacity) {
        errno = EMSGSIZE;
        return -1;
    }

    return n;
}

/*
 * 读取 AP3216C ALS 原始值。
 *
 * 返回：
 *   0  成功，结果存入 out
 *  -1  失败，通过 errno 获取原因
 */
static int read_als_raw(unsigned int *out)
{
    FILE *fp;
    char buf[64];
    char *end;
    unsigned long value;
    int saved_errno;

    if (out == NULL) {
        errno = EINVAL;
        return -1;
    }

    fp = fopen(ALS_PATH, "r");
    if (fp == NULL)
        return -1;

    errno = 0;

    if (fgets(buf, sizeof(buf), fp) == NULL) {
        saved_errno = ferror(fp) ? errno : EIO;
        fclose(fp);
        errno = saved_errno ? saved_errno : EIO;
        return -1;
    }

    if (fclose(fp) != 0)
        return -1;

    errno = 0;
    value = strtoul(buf, &end, 10);

    if (errno == ERANGE ||
        end == buf ||
        value > 65535UL) {
        errno = EINVAL;
        return -1;
    }

    while (*end == ' ' ||
           *end == '\n' ||
           *end == '\r' ||
           *end == '\t') {
        end++;
    }

    if (*end != '\0') {
        errno = EINVAL;
        return -1;
    }

    *out = (unsigned int)value;
    return 0;
}




int main(void)
{
    int tcp_fd = -1;
    int exit_code = EXIT_FAILURE;

    uint32_t tx_seq = 0;

    int64_t next_sample;
    int64_t next_heartbeat;

    /*
     * 传感器状态：
     * -1 = 尚未采集
     *  0 = 最近一次采集失败
     *  1 = 最近一次采集成功
     */
    int sensor_ok = -1;

    /*
     * 单条待发送消息缓存。
     *
     * pending_len == 0:
     *     当前没有待发送消息。
     *
     * pending_len > 0:
     *     当前消息尚未完成发送。
     */
    char pending_json[256];
    int pending_len = 0;

    /* 标记待发送消息是否为心跳 */
    int pending_is_heartbeat = 0;

    /*
     * 1. 安装 SIGINT / SIGTERM 信号处理。
     */
    if (install_signals() != 0) {
        perror("install_signals");
        return EXIT_FAILURE;
    }

    /*
     * 2. 初始化单调时钟。
     */
    next_sample = monotonic_ms();

    if (next_sample < 0) {
        perror("clock_gettime");
        return EXIT_FAILURE;
    }

    /*
     * 第一次采集立即执行。
     * 第一次心跳在 10 秒后执行。
     */
    next_heartbeat =
        next_sample + HEARTBEAT_PERIOD_MS;

    printf(
        "[SENSOR] NODE04 started, "
        "sample=%dms heartbeat=%dms\n",
        SAMPLE_PERIOD_MS,
        HEARTBEAT_PERIOD_MS
    );

    fflush(stdout);

    /*
     * 3. 主循环。
     */
    while (g_running) {

        /*
         * =================================================
         * A. 没有 pending 时才允许生成新消息。
         * =================================================
         */
        if (pending_len == 0) {

            int64_t now = monotonic_ms();

            if (now < 0) {
                perror("clock_gettime");
                goto cleanup;
            }

            /*
             * ---------------------------------------------
             * A1. 心跳到期：优先生成心跳。
             * ---------------------------------------------
             */
            if (now >= next_heartbeat) {

                const char *status;
                int length;

                if (sensor_ok < 0) {
                    status = "UNKNOWN";
                }
                else if (sensor_ok == 0) {
                    status = "ERROR";
                }
                else {
                    status = "OK";
                }

                length = build_heartbeat_json(
                    pending_json,
                    sizeof(pending_json),
                    tx_seq + 1U,
                    status
                );

                if (length < 0) {
                    perror("build_heartbeat_json");
                    goto cleanup;
                }

                /*
                 * JSON 构造成功后才增加序号。
                 */
                tx_seq++;

                pending_len = length;
                pending_is_heartbeat = 1;

                /*
                 * 推进下一次心跳时间。
                 *
                 * 如果严重超时，跳过遗漏周期，
                 * 避免短时间连续补发过期心跳。
                 */
                next_heartbeat += HEARTBEAT_PERIOD_MS;

                if (next_heartbeat <= now) {
                    next_heartbeat =
                        now + HEARTBEAT_PERIOD_MS;
                }

                printf(
                    "[SENSOR] HEARTBEAT prepared "
                    "seq=%u SENSOR=%s\n",
                    (unsigned int)tx_seq,
                    status
                );
            }

            /*
             * ---------------------------------------------
             * A2. 采集到期：读取 AP3216C。
             *
             * 使用 else if，避免覆盖刚生成的心跳。
             * ---------------------------------------------
             */
            else if (now >= next_sample) {

                unsigned int als = 0;
                int length;

                /*
                 * 推进下一次采样时间。
                 */
                next_sample += SAMPLE_PERIOD_MS;

                if (next_sample <= now) {
                    next_sample =
                        now + SAMPLE_PERIOD_MS;
                }

                /*
                 * 真正读取内核 sysfs 属性。
                 */
                if (read_als_raw(&als) < 0) {

                    /*
                     * 记录最近一次采集失败。
                     *
                     * 不增加 tx_seq。
                     * 不生成虚假的 SAMPLE。
                     * 不退出进程。
                     */
                    sensor_ok = 0;

                    fprintf(
                        stderr,
                        "[SENSOR] ALS read failed: %s\n",
                        strerror(errno)
                    );

                    continue;
                }

                /*
                 * ALS 读取成功。
                 */
                sensor_ok = 1;

                /*
                 * 构造 SENSOR SAMPLE 消息。
                 */
                length = build_sensor_json(
                    pending_json,
                    sizeof(pending_json),
                    tx_seq + 1U,
                    als
                );

                if (length < 0) {
                    perror("build_sensor_json");
                    goto cleanup;
                }

                tx_seq++;

                pending_len = length;
                pending_is_heartbeat = 0;

                printf(
                    "[SENSOR] ALS=%u seq=%u\n",
                    als,
                    (unsigned int)tx_seq
                );
            }

            /*
             * ---------------------------------------------
             * A3. 采样和心跳都没到期：等待。
             * ---------------------------------------------
             */
            else {

                int64_t sample_wait;
                int64_t heartbeat_wait;
                int64_t wait_ms;
                int timeout;

                sample_wait =
                    next_sample - now;

                heartbeat_wait =
                    next_heartbeat - now;

                /*
                 * 取两个任务中更早的截止时间。
                 */
                if (sample_wait < heartbeat_wait) {
                    wait_ms = sample_wait;
                }
                else {
                    wait_ms = heartbeat_wait;
                }

                timeout = (int)wait_ms;

                /*
                 * 等待期间可以被 SIGINT/SIGTERM
                 * 中断，不进行忙轮询。
                 */
                (void)poll(NULL, 0, timeout);

                continue;
            }
        }

        /*
         * =================================================
         * B. 有待发送消息，检查 TCP 连接。
         * =================================================
         */
        if (tcp_fd < 0) {

            tcp_fd = connect_gateway();

            if (tcp_fd < 0) {

                if (!g_running) {
                    break;
                }

                perror("[SENSOR] connect Gateway");

                /*
                 * 重连期间不能覆盖 pending。
                 */
                (void)poll(
                    NULL,
                    0,
                    RETRY_INTERVAL_MS
                );

                continue;
            }

            puts("[SENSOR] Gateway connected");
            fflush(stdout);
        }

        /*
         * =================================================
         * C. 发送完整 JSON Lines 消息。
         * =================================================
         */
        if (send_all_timeout(
                tcp_fd,
                pending_json,
                (size_t)pending_len) != 0) {

            if (!g_running) {
                break;
            }

            perror("[SENSOR] send failed");

            /*
             * 当前连接异常：
             * 关闭旧 socket。
             */
            close(tcp_fd);
            tcp_fd = -1;

            /*
             * 不清空 pending_json。
             *
             * 下次连接成功后，原样重发
             * 当前消息及原来的 seq。
             */
            (void)poll(
                NULL,
                0,
                RETRY_INTERVAL_MS
            );

            continue;
        }

        /*
         * =================================================
         * D. 当前消息已完整提交给本地 TCP。
         * =================================================
         */
        if (pending_is_heartbeat) {
            printf(
                "[SENSOR] HEARTBEAT TX: %s",
                pending_json
            );
        }
        else {
            printf(
                "[SENSOR] SAMPLE TX: %s",
                pending_json
            );
        }

        fflush(stdout);

        /*
         * 注意：
         * 这里的发送成功只表示 send_all_timeout()
         * 成功，不等于 Server 已完成业务处理。
         */

        /*
         * 允许下一轮生成新消息。
         */
        pending_len = 0;
        pending_is_heartbeat = 0;
    }

    /*
     * Ctrl+C 等正常退出。
     */
    exit_code = EXIT_SUCCESS;

cleanup:

    /*
     * 统一释放 TCP 资源。
     */
    if (tcp_fd >= 0) {
        close(tcp_fd);
        tcp_fd = -1;
    }

    puts("[SENSOR] stopped");

    return exit_code;
}

