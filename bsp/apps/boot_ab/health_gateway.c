#define _GNU_SOURCE
#include "health_gateway.h"
#include "health_supervisor.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, long long deadline)
{
    for (;;) {
        struct pollfd pfd = {fd, events, 0};
        long long now = now_ms();
        int rc;
        if (now < 0 || now >= deadline) return -1;
        rc = poll(&pfd, 1, (int)(deadline - now));
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) return -1;
        if (pfd.revents & events) return 0;
        if (pfd.revents & POLLHUP) return -1;
    }
}

int sgw_health_gateway_probe(const char *socket_path, uid_t trusted_uid,
                             pid_t expected_pid, unsigned long long expected_start,
                             int timeout_ms)
{
    unsigned char random[16];
    char nonce[33], request[64], expected[80], response[80];
    static const char hex[] = "0123456789abcdef";
    struct sockaddr_un addr;
    struct ucred peer;
    socklen_t len;
    pid_t parent;
    unsigned long long started;
    char state;
    int fd = -1, rc = -1, error = 0, n;
    size_t i, sent = 0, used = 0, request_len, expected_len;
    long long start, deadline;

    if (!socket_path || expected_pid <= 1 || expected_start == 0 ||
        timeout_ms <= 0 || timeout_ms > 10000 ||
        strlen(socket_path) >= sizeof(addr.sun_path) ||
        sgw_health_trusted_path(socket_path, trusted_uid, 1) != 0 ||
        sgw_health_process_snapshot(expected_pid, &state, &parent, &started) != 0 ||
        started != expected_start || state == 'Z' || state == 'X' || state == 'x' ||
        state == 'T' || state == 't') return -1;
    /* A predictable nonce is not acceptable; lack of entropy is UNKNOWN. */
    if (getrandom(random, sizeof(random), GRND_NONBLOCK) != (ssize_t)sizeof(random))
        return -1;
    for (i = 0; i < sizeof(random); ++i) {
        nonce[i * 2] = hex[random[i] >> 4];
        nonce[i * 2 + 1] = hex[random[i] & 15];
    }
    nonce[32] = '\0';
    n = snprintf(request, sizeof(request), "SGW-PROBE/1 %s\n", nonce);
    if (n <= 0 || (size_t)n >= sizeof(request)) return -1;
    request_len = (size_t)n;
    n = snprintf(expected, sizeof(expected), "SGW-PROBE/1 %s READY\n", nonce);
    if (n <= 0 || (size_t)n >= sizeof(expected)) return -1;
    expected_len = (size_t)n;
    start = now_ms();
    if (start < 0) return -1;
    deadline = start + timeout_ms;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, socket_path, strlen(socket_path) + 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS || wait_fd(fd, POLLOUT, deadline) != 0) goto out;
        len = sizeof(error);
        if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len) != 0 || error) goto out;
    }
    len = sizeof(peer);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &peer, &len) != 0 ||
        len != sizeof(peer) || peer.uid != trusted_uid || peer.pid != expected_pid)
        goto out;
    while (sent < request_len) {
        ssize_t wrote;
        if (wait_fd(fd, POLLOUT, deadline) != 0) goto out;
        wrote = send(fd, request + sent, request_len - sent, MSG_NOSIGNAL);
        if (wrote < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (wrote <= 0) goto out;
        sent += (size_t)wrote;
    }
    while (used < sizeof(response)) {
        ssize_t got;
        if (wait_fd(fd, POLLIN, deadline) != 0) goto out;
        got = recv(fd, response + used, 1, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) goto out;
        if (response[used++] == '\n') break;
    }
    if (used != expected_len || memcmp(response, expected, expected_len) != 0 ||
        sgw_health_process_snapshot(expected_pid, &state, &parent, &started) != 0 ||
        started != expected_start || state == 'Z' || state == 'X' || state == 'x' ||
        state == 'T' || state == 't') goto out;
    rc = 0;
out:
    if (fd >= 0) close(fd);
    return rc;
}
