#define _GNU_SOURCE
#include "health_probe.h"
#include "message_json.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define QUERY_PREFIX "SGW-PROBE/1 "
#define FLOW_PREFIX  "SGW-FLOWX/1 "
#define NONCE_LEN 32
#define PROBE_TIMEOUT_MS 200
#define FLOW_MAX_AGE_SECONDS 5u

_Static_assert(sizeof(QUERY_PREFIX) == sizeof(FLOW_PREFIX), "query prefix lengths");
static atomic_uint last_forwarded_seconds;

void gateway_health_observe_forwarded(const unsigned char *data, size_t length)
{
    frame_data_t frame;
    struct timespec ts;
    if (!data || length == 0 ||
        message_json_decode_data((const char *)data, length, &frame) < 0 ||
        clock_gettime(CLOCK_MONOTONIC, &ts) != 0 ||
        ts.tv_sec <= 0 || (unsigned long long)ts.tv_sec > UINT_MAX) return;
    atomic_store_explicit(&last_forwarded_seconds, (unsigned int)ts.tv_sec,
                          memory_order_release);
}

static int recent_forwarded(void)
{
    struct timespec ts;
    unsigned int last = atomic_load_explicit(&last_forwarded_seconds, memory_order_acquire);
    if (last == 0 || clock_gettime(CLOCK_MONOTONIC, &ts) != 0 ||
        ts.tv_sec < 0 || (unsigned long long)ts.tv_sec > UINT_MAX ||
        (unsigned int)ts.tv_sec < last) return 0;
    return (unsigned int)ts.tv_sec - last <= FLOW_MAX_AGE_SECONDS;
}

/* No environment override is present in a production build. */
static const char *health_directory(void)
{
#ifdef SGW_GATEWAY_HEALTH_TESTING
    const char *value = getenv("SGW_GATEWAY_HEALTH_TEST_DIR");
    if (value && *value) return value;
#endif
    return "/opt/smartgateway/run";
}

static uid_t health_uid(void)
{
#ifdef SGW_GATEWAY_HEALTH_TESTING
    return geteuid();
#else
    return 0;
#endif
}

static int trusted_directory(const char *path, uid_t uid)
{
    char partial[108];
    size_t i, n;
    struct stat st;

    if (!path || path[0] != '/') return -1;
    n = strlen(path);
    if (n == 0 || n >= sizeof(partial)) return -1;
    if (lstat("/", &st) != 0 || st.st_uid != 0 ||
        (st.st_mode & (S_IWGRP | S_IWOTH))) return -1;
    memcpy(partial, path, n + 1);
    for (i = 1; i <= n; ++i) {
        char saved;
        if (partial[i] != '/' && partial[i] != '\0') continue;
        if (partial[i - 1] == '/') return -1;
        saved = partial[i];
        partial[i] = '\0';
        if (lstat(partial, &st) != 0 || !S_ISDIR(st.st_mode) ||
            (st.st_uid != 0 && st.st_uid != uid) ||
            (st.st_mode & (S_IWGRP | S_IWOTH))) return -1;
        partial[i] = saved;
    }
    return 0;
}

void gateway_health_listener_init(gateway_health_listener_t *listener)
{
    if (!listener) return;
    memset(listener, 0, sizeof(*listener));
    listener->fd = -1;
}

int gateway_health_listener_open(gateway_health_listener_t *listener)
{
    const char *dir = health_directory();
    struct sockaddr_un addr;
    struct stat st;
    mode_t old_mask;
    int fd, n;

    if (!listener || geteuid() != health_uid() ||
        trusted_directory(dir, health_uid()) != 0) return -1;
    n = snprintf(listener->path, sizeof(listener->path), "%s/gateway-health.sock", dir);
    if (n < 0 || (size_t)n >= sizeof(listener->path)) return -1;
    if (lstat(listener->path, &st) == 0 || errno != ENOENT) return -1;
    fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, listener->path, strlen(listener->path) + 1);

    /* Called before Gateway creates worker threads. */
    old_mask = umask(0177);
    n = bind(fd, (struct sockaddr *)&addr, sizeof(addr));
    umask(old_mask);
    if (n != 0) { close(fd); return -1; }
    if (lstat(listener->path, &st) != 0 || !S_ISSOCK(st.st_mode) ||
        st.st_uid != health_uid()) {
        close(fd);
        return -1;
    }
    listener->socket_device = (unsigned long long)st.st_dev;
    listener->socket_inode = (unsigned long long)st.st_ino;
    listener->fd = fd;
    if ((st.st_mode & 077) || listen(fd, 4) != 0) {
        gateway_health_listener_close(listener);
        return -1;
    }
    return 0;
}

static long long now_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return -1;
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Actual DATA parsing/serialization and live business structures, without
 * registering a node, consuming an event, or touching the upstream queue. */
static int business_components_ready(const char *nonce, message_queue_t *queue,
                                     device_manager_t *devices)
{
    char in[192], out[256];
    frame_data_t decoded, roundtrip;
    unsigned int seq = 0;
    int n, encoded, queue_ok, manager_ok;
    size_t i;

    if (!nonce || !queue || !devices) return 0;
    for (i = 0; i < 8; ++i)
        seq = (seq << 4) | (unsigned int)(isdigit((unsigned char)nonce[i]) ?
              nonce[i] - '0' : 10 + tolower((unsigned char)nonce[i]) - 'a');
    seq %= 900000000u;
    n = snprintf(in, sizeof(in),
        "{\"node\":\"OTA01\",\"seq\":%u,\"type\":\"DATA\","
        "\"fields\":{\"T\":\"253\",\"H\":\"601\"}}", seq);
    if (n <= 0 || (size_t)n >= sizeof(in) ||
        message_json_decode_data(in, (size_t)n, &decoded) < 0) return 0;
    encoded = message_json_build_data(out, sizeof(out), &decoded);
    if (encoded <= 0 || message_json_decode_data(out, (size_t)encoded, &roundtrip) < 0 ||
        strcmp(roundtrip.node_id, "OTA01") != 0 || roundtrip.sequence != seq ||
        !frame_data_find_field(&roundtrip, "T") ||
        strcmp(frame_data_find_field(&roundtrip, "T"), "253") != 0 ||
        !frame_data_find_field(&roundtrip, "H") ||
        strcmp(frame_data_find_field(&roundtrip, "H"), "601") != 0) return 0;

    if (pthread_mutex_trylock(&queue->mutex) != 0) return 0;
    queue_ok = !queue->shutting_down && queue->count < MESSAGE_QUEUE_CAPACITY &&
               queue->head < MESSAGE_QUEUE_CAPACITY && queue->tail < MESSAGE_QUEUE_CAPACITY;
    pthread_mutex_unlock(&queue->mutex);
    if (pthread_mutex_trylock(&devices->mutex) != 0) return 0;
    manager_ok = devices->count <= DEVICE_MANAGER_MAX_DEVICES;
    pthread_mutex_unlock(&devices->mutex);
    return queue_ok && manager_ok;
}

void gateway_health_listener_service(gateway_health_listener_t *listener,
                                     message_queue_t *queue,
                                     device_manager_t *devices)
{
    int client;
    struct ucred peer;
    socklen_t peer_len = sizeof(peer);
    char request[sizeof(QUERY_PREFIX) - 1 + NONCE_LEN + 2], response[96];
    size_t used = 0, i;
    long long deadline;

    if (!listener || listener->fd < 0) return;
    client = accept4(listener->fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
    if (client < 0) return;
    if (getsockopt(client, SOL_SOCKET, SO_PEERCRED, &peer, &peer_len) != 0 ||
        peer_len != sizeof(peer) || peer.uid != health_uid()) { close(client); return; }
    deadline = now_ms();
    if (deadline < 0) { close(client); return; }
    deadline += PROBE_TIMEOUT_MS;
    while (used < sizeof(request) - 1) {
        struct pollfd pfd = {client, POLLIN, 0};
        long long now = now_ms();
        ssize_t got;
        int r;
        if (now < 0 || now >= deadline) break;
        r = poll(&pfd, 1, (int)(deadline - now));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || !(pfd.revents & POLLIN)) break;
        got = recv(client, request + used, 1, 0);
        if (got < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (got <= 0) break;
        if (request[used++] == '\n') break;
    }
    if (used != sizeof(QUERY_PREFIX) - 1 + NONCE_LEN + 1 ||
        request[used - 1] != '\n' ||
        (memcmp(request, QUERY_PREFIX, sizeof(QUERY_PREFIX) - 1) != 0 &&
         memcmp(request, FLOW_PREFIX, sizeof(FLOW_PREFIX) - 1) != 0)) {
        close(client);
        return;
    }
    request[used - 1] = '\0';
    for (i = sizeof(QUERY_PREFIX) - 1; i < used - 1; ++i)
        if (!isxdigit((unsigned char)request[i])) { close(client); return; }

    {
        const char *nonce = request + sizeof(QUERY_PREFIX) - 1;
        int flow = memcmp(request, FLOW_PREFIX, sizeof(FLOW_PREFIX) - 1) == 0;
        int n = snprintf(response, sizeof(response), "%s%s %s\n",
                         flow ? FLOW_PREFIX : QUERY_PREFIX, nonce,
                         flow ? (recent_forwarded() ? "FORWARDED" : "UNKNOWN") :
                         (business_components_ready(nonce, queue, devices) ?
                          "READY" : "NOT_READY"));
        if (n > 0 && (size_t)n < sizeof(response))
            (void)send(client, response, (size_t)n, MSG_NOSIGNAL | MSG_DONTWAIT);
    }
    close(client);
}

void gateway_health_listener_close(gateway_health_listener_t *listener)
{
    struct stat st;
    if (!listener || listener->fd < 0) return;
    close(listener->fd);
    listener->fd = -1;
    if (lstat(listener->path, &st) == 0 && S_ISSOCK(st.st_mode) &&
        (unsigned long long)st.st_dev == listener->socket_device &&
        (unsigned long long)st.st_ino == listener->socket_inode)
        (void)unlink(listener->path);
}
