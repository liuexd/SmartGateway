#define _GNU_SOURCE
#include "health_supervisor.h"
#include "service_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* Check both original and resolved paths, including symlink ownership. */
static int path_components(const char *path, uid_t uid, int socket_file,
                           int allow_symlinks)
{
    char partial[PATH_MAX];
    size_t i, len;
    struct stat st;

    if (!path || path[0] != '/') return -1;
    len = strlen(path);
    if (len == 0 || len >= sizeof(partial)) return -1;
    if (lstat("/", &st) != 0 || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH)) != 0) return -1;
    memcpy(partial, path, len + 1);
    for (i = 1; i <= len; ++i) {
        char saved;
        if (partial[i] != '/' && partial[i] != '\0') continue;
        if (partial[i - 1] == '/') return -1;
        saved = partial[i];
        partial[i] = '\0';
        if (lstat(partial, &st) != 0 ||
            (st.st_uid != 0 && st.st_uid != uid)) return -1;
        if (S_ISLNK(st.st_mode)) {
            if (!allow_symlinks) return -1;
        } else if ((st.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
                   (i != len && !S_ISDIR(st.st_mode)) ||
                   (i == len && (socket_file ? !S_ISSOCK(st.st_mode) : !S_ISREG(st.st_mode)))) {
            return -1;
        }
        partial[i] = saved;
    }
    return 0;
}

static int trusted_path(const char *path, uid_t uid, int socket_file)
{
    char resolved[PATH_MAX];
    if (!path || path_components(path, uid, socket_file, 1) != 0 ||
        !realpath(path, resolved)) return -1;
    return path_components(resolved, uid, socket_file, 0);
}

static int same_process_executable(pid_t pid, const char *expected)
{
    char path[64];
    struct stat live, file;
    int n = snprintf(path, sizeof(path), "/proc/%ld/exe", (long)pid);
    return n > 0 && (size_t)n < sizeof(path) && stat(path, &live) == 0 &&
           stat(expected, &file) == 0 && live.st_dev == file.st_dev &&
           live.st_ino == file.st_ino;
}

static int process_snapshot(pid_t pid, char *state, pid_t *ppid,
                            unsigned long long *start)
{
    char path[64], buf[2048], *end, *token, *save = NULL;
    FILE *fp;
    int field;
    unsigned long long val;

    if (snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid) >= (int)sizeof(path))
        return -1;
    fp = fopen(path, "r");
    if (!fp) return -1;
    if (!fgets(buf, sizeof(buf), fp)) { fclose(fp); return -1; }
    fclose(fp);
    end = strrchr(buf, ')');
    if (!end || end[1] != ' ') return -1;
    token = strtok_r(end + 2, " ", &save);
    if (!token || strlen(token) != 1) return -1;
    *state = token[0];
    for (field = 4; field <= 22; ++field) {
        token = strtok_r(NULL, " ", &save);
        if (!token) return -1;
        if (field == 4 || field == 22) {
            char *tail;
            errno = 0;
            val = strtoull(token, &tail, 10);
            if (errno || tail == token || (*tail && *tail != '\n')) return -1;
            if (field == 4) {
                if (val == 0 || val > INT_MAX) return -1;
                *ppid = (pid_t)val;
            } else {
                *start = val;
            }
        }
    }
    return *start == 0 ? -1 : 0;
}

static long long monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int wait_fd(int fd, short events, long long deadline)
{
    for (;;) {
        struct pollfd pfd = {fd, events, 0};
        long long now = monotonic_ms();
        int rc, remaining;
        if (now < 0 || now >= deadline) { errno = ETIMEDOUT; return -1; }
        remaining = (int)(deadline - now);
        rc = poll(&pfd, 1, remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) { if (rc == 0) errno = ETIMEDOUT; return -1; }
        if (pfd.revents & (POLLERR | POLLNVAL)) return -1;
        if (pfd.revents & events) return 0;
        if (pfd.revents & POLLHUP) return -1;
    }
}

int sgw_health_supervisor_status(const char *socket_path,
                                 const char *supervisor_executable,
                                 const char *gateway_executable,
                                 uid_t trusted_uid,
                                 int timeout_ms,
                                 pid_t *gateway_pid)
{
    struct sockaddr_un addr;
    struct ucred peer;
    socklen_t len = sizeof(peer);
    service_ipc_reply_t reply;
    char line[256], state, state2, peer_state, peer_state2;
    pid_t parent, parent2, peer_parent, peer_parent2;
    unsigned long long started, started2, peer_start, peer_start2;
    int fd = -1, error = 0, rc = -1;
    long long start, deadline;
    size_t used = 0, sent = 0;
    static const char request[] = "STATUS\n";

    if (gateway_pid) *gateway_pid = 0;
    if (!socket_path || !supervisor_executable || !gateway_executable ||
        timeout_ms <= 0 || timeout_ms > 10000 ||
        strlen(socket_path) >= sizeof(addr.sun_path) ||
        trusted_path(socket_path, trusted_uid, 1) != 0 ||
        trusted_path(supervisor_executable, trusted_uid, 0) != 0 ||
        trusted_path(gateway_executable, trusted_uid, 0) != 0)
        return -1;
    start = monotonic_ms();
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
        len != sizeof(peer) || peer.uid != trusted_uid || peer.pid <= 1 ||
        !same_process_executable(peer.pid, supervisor_executable) ||
        process_snapshot(peer.pid, &peer_state, &peer_parent, &peer_start) != 0 ||
        peer_state == 'Z' || peer_state == 'X' || peer_state == 'x') goto out;

    while (sent < sizeof(request) - 1) {
        ssize_t n;
        if (wait_fd(fd, POLLOUT, deadline) != 0) goto out;
        n = send(fd, request + sent, sizeof(request) - 1 - sent, MSG_NOSIGNAL);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) goto out;
        sent += (size_t)n;
    }
    while (used + 1 < sizeof(line)) {
        ssize_t n;
        if (wait_fd(fd, POLLIN, deadline) != 0) goto out;
        n = recv(fd, line + used, 1, 0);
        if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (n <= 0) goto out;
        if (line[used++] == '\n') break;
    }
    if (used == 0 || line[used - 1] != '\n') goto out;
    line[used] = '\0';
    if (service_ipc_decode_reply(line, &reply) != 0 ||
        reply.result != SERVICE_IPC_OK || reply.pid <= 1 || reply.starttime == 0 ||
        process_snapshot(reply.pid, &state, &parent, &started) != 0 ||
        state == 'Z' || state == 'X' || state == 'x' ||
        parent != peer.pid || started != reply.starttime ||
        !same_process_executable(reply.pid, gateway_executable) ||
        process_snapshot(reply.pid, &state2, &parent2, &started2) != 0 ||
        state2 == 'Z' || state2 == 'X' || state2 == 'x' ||
        parent2 != parent || started2 != started ||
        process_snapshot(peer.pid, &peer_state2, &peer_parent2, &peer_start2) != 0 ||
        peer_state2 == 'Z' || peer_state2 == 'X' || peer_state2 == 'x' ||
        peer_parent2 != peer_parent || peer_start2 != peer_start ||
        !same_process_executable(peer.pid, supervisor_executable) ||
        !same_process_executable(reply.pid, gateway_executable))
        goto out;
    if (gateway_pid) *gateway_pid = reply.pid;
    rc = 0;
out:
    if (fd >= 0) close(fd);
    return rc;
}
