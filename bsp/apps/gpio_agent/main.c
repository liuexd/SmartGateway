#define _POSIX_C_SOURCE 200809L

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <poll.h>

#ifndef GPIO_DEVICE
#define GPIO_DEVICE          "/dev/sgw_event0"
#endif
#ifndef GATEWAY_IP
#define GATEWAY_IP           "127.0.0.1"
#endif
#ifndef GATEWAY_PORT
#define GATEWAY_PORT         7000
#endif
#define CONNECT_TIMEOUT_MS   2000
#define SEND_TIMEOUT_MS      1000
#define RETRY_INTERVAL_MS    2000
#define GPIO_POLL_MS         1000
#define HEARTBEAT_MS        10000

/* 与内核驱动输出保持一致；后续统一迁移到共享 UAPI 头文件。 */
struct sgw_gpio_event {
    uint64_t timestamp_ns;
    uint32_t sequence;
    uint32_t level;
};
_Static_assert(sizeof(struct sgw_gpio_event) == 16,
               "sgw_gpio_event ABI mismatch");

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

/* NODE03 的应用层消息序号独立于内核 EVT_SEQ。 */
static int format_event(char *out, size_t cap, uint32_t tx_seq,
                        const struct sgw_gpio_event *ev)
{
    int n;
    const char *state;
    if (ev->level > 1U) {
        errno = EPROTO;
        return -1;
    }
    state = ev->level == 0U ? "PRESSED" : "RELEASED";
    n = snprintf(out, cap,
                 "{\"node\":\"NODE03\",\"seq\":%u,\"type\":\"DATA\","
                 "\"fields\":{\"DEV\":\"4\",\"GPIO\":\"115\","
                 "\"KIND\":\"EDGE\",\"LEVEL\":\"%u\",\"STATE\":\"%s\","
                 "\"EVT_SEQ\":\"%u\",\"TS_NS\":\"%" PRIu64 "\"}}\n",
                 (unsigned int)tx_seq, (unsigned int)ev->level,
                 state, (unsigned int)ev->sequence, ev->timestamp_ns);
    if (n < 0 || (size_t)n >= cap) {
        errno = EMSGSIZE;
        return -1;
    }
    return n;
}

/* 心跳仍使用现有 DATA 协议，以便 Server 刷新 last_update。 */
static int format_heartbeat(char *out, size_t cap, uint32_t tx_seq,
                            int have_level, uint32_t level)
{
    int n;
    if (have_level) {
        const char *state = level == 0U ? "PRESSED" : "RELEASED";
        n = snprintf(out, cap,
                     "{\"node\":\"NODE03\",\"seq\":%u,\"type\":\"DATA\","
                     "\"fields\":{\"DEV\":\"4\",\"GPIO\":\"115\","
                     "\"KIND\":\"HEARTBEAT\",\"STATUS\":\"ALIVE\","
                     "\"LEVEL\":\"%u\",\"STATE\":\"%s\"}}\n",
                     (unsigned int)tx_seq, (unsigned int)level, state);
    } else {
        n = snprintf(out, cap,
                     "{\"node\":\"NODE03\",\"seq\":%u,\"type\":\"DATA\","
                     "\"fields\":{\"DEV\":\"4\",\"GPIO\":\"115\","
                     "\"KIND\":\"HEARTBEAT\",\"STATUS\":\"ALIVE\"}}\n",
                     (unsigned int)tx_seq);
    }
    if (n < 0 || (size_t)n >= cap) {
        errno = EMSGSIZE;
        return -1;
    }
    return n;
}

int main(void)
{
    int gpio_fd = -1, tcp_fd = -1;
    int exit_code = EXIT_FAILURE;
    int have_pending = 0, pending_is_heartbeat = 0;
    int have_event_seq = 0, have_level = 0;
    uint32_t last_event_seq = 0, last_level = 0, tx_seq = 0;
    int64_t heartbeat_due = 0; /* 首次连接成功后立即上报状态 */
    char pending_json[256];
    int pending_length = 0;
    struct pollfd pfd;

    if (install_signals() != 0) {
        perror("sigaction");
        return EXIT_FAILURE;
    }
    gpio_fd = open(GPIO_DEVICE, O_RDONLY | O_NONBLOCK);
    if (gpio_fd < 0) {
        perror("open GPIO device");
        return EXIT_FAILURE;
    }
    pfd.fd = gpio_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    printf("[GPIO AGENT] device=%s gateway=%s:%d heartbeat=%dms\n",
           GPIO_DEVICE, GATEWAY_IP, GATEWAY_PORT, HEARTBEAT_MS);
    fflush(stdout);

    while (g_running) {
        if (tcp_fd < 0) {
            tcp_fd = connect_gateway();
            if (tcp_fd < 0) {
                if (!g_running)
                    break;
                perror("[GPIO AGENT] connect Gateway");
                (void)poll(NULL, 0, RETRY_INTERVAL_MS);
                continue;
            }
            puts("[GPIO AGENT] connected to Gateway");
            fflush(stdout);
        }

        if (!have_pending) {
            int64_t now = monotonic_ms();
            if (now < 0) {
                perror("clock_gettime");
                goto cleanup;
            }
            if (now >= heartbeat_due) {
                /* 上报心跳优先于下一条新事件；已读取的 pending 不会被覆盖。 */
                tx_seq++;
                pending_length = format_heartbeat(pending_json,
                                                   sizeof(pending_json),
                                                   tx_seq, have_level,
                                                   last_level);
                pending_is_heartbeat = 1;
                if (pending_length < 0) {
                    perror("format heartbeat");
                    goto cleanup;
                }
                have_pending = 1;
            } else {
                int ret;
                ssize_t n;
                int timeout = GPIO_POLL_MS;
                int64_t remain = heartbeat_due - now;
                struct sgw_gpio_event ev;
                if (remain < timeout)
                    timeout = (int)remain;
                pfd.revents = 0;
                ret = poll(&pfd, 1, timeout);
                if (ret < 0) {
                    if (errno == EINTR)
                        continue;
                    perror("poll GPIO");
                    goto cleanup;
                }
                if (ret == 0)
                    continue;
                if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
                    fprintf(stderr, "[GPIO AGENT] GPIO device unavailable\n");
                    goto cleanup;
                }
                if (!(pfd.revents & POLLIN))
                    continue;

                n = read(gpio_fd, &ev, sizeof(ev));
                if (n < 0 && (errno == EAGAIN || errno == EINTR))
                    continue;
                if (n != (ssize_t)sizeof(ev)) {
                    fprintf(stderr, "[GPIO AGENT] GPIO short read/error: %zd\n", n);
                    goto cleanup;
                }
                if (have_event_seq && ev.sequence != last_event_seq + 1U) {
                    fprintf(stderr,
                            "[GPIO AGENT] event gap: prev=%u now=%u\n",
                            (unsigned int)last_event_seq,
                            (unsigned int)ev.sequence);
                }
                last_event_seq = ev.sequence;
                have_event_seq = 1;
                if (ev.level > 1U) {
                    fprintf(stderr, "[GPIO AGENT] invalid GPIO level=%u\n",
                            (unsigned int)ev.level);
                    goto cleanup;
                }
                have_level = 1;
                last_level = ev.level;
                tx_seq++;
                pending_length = format_event(pending_json,
                                              sizeof(pending_json), tx_seq, &ev);
                pending_is_heartbeat = 0;
                if (pending_length < 0) {
                    perror("format event");
                    goto cleanup;
                }
                have_pending = 1;
            }
        }

        if (send_all_timeout(tcp_fd, pending_json,
                             (size_t)pending_length) != 0) {
            if (!g_running)
                break;
            perror("[GPIO AGENT] send Gateway");
            close(tcp_fd);
            tcp_fd = -1;
            /* 保留 JSON 和应用层 seq，重连后原样重试。 */
            (void)poll(NULL, 0, RETRY_INTERVAL_MS);
            continue;
        }
        printf("[GPIO AGENT] TX: %s", pending_json);
        fflush(stdout);
        if (pending_is_heartbeat) {
            int64_t now = monotonic_ms();
            if (now < 0) {
                perror("clock_gettime");
                goto cleanup;
            }
            heartbeat_due = now + HEARTBEAT_MS;
        }
        have_pending = 0;
    }
    exit_code = EXIT_SUCCESS;

cleanup:
    if (tcp_fd >= 0)
        close(tcp_fd);
    close(gpio_fd);
    puts("[GPIO AGENT] stopped");
    return exit_code;
}
