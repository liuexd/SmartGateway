#define _GNU_SOURCE
#include "health_probe.h"
#include "message_json.h"

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
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
#define LOCAL_PREFIX "SGW-LOCAL/1 "
#define LOCAL_MARKER_PREFIX "SGW-LOCAL-MARKER:"
#define NONCE_LEN 32
#define PROBE_TIMEOUT_MS 200
#define FLOW_MAX_AGE_SECONDS 5u

_Static_assert(sizeof(QUERY_PREFIX) == sizeof(FLOW_PREFIX), "query prefix lengths");
_Static_assert(sizeof(QUERY_PREFIX) == sizeof(LOCAL_PREFIX), "local prefix lengths");
static atomic_uint last_forwarded_seconds;
static atomic_uint last_worker_tick_seconds;
static pthread_mutex_t local_diag_mutex = PTHREAD_MUTEX_INITIALIZER;
static char pending_local_nonce[NONCE_LEN + 1];
static char consumed_local_nonce[NONCE_LEN + 1];

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

void gateway_health_observe_worker_tick(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 ||
        ts.tv_sec <= 0 || (unsigned long long)ts.tv_sec > UINT_MAX) return;
    atomic_store_explicit(&last_worker_tick_seconds, (unsigned int)ts.tv_sec,
                          memory_order_release);
}

static int recent_timestamp(atomic_uint *stamp)
{
    struct timespec ts;
    unsigned int last = atomic_load_explicit(stamp, memory_order_acquire);
    if (last == 0 || clock_gettime(CLOCK_MONOTONIC, &ts) != 0 ||
        ts.tv_sec < 0 || (unsigned long long)ts.tv_sec > UINT_MAX ||
        (unsigned int)ts.tv_sec < last) return 0;
    return (unsigned int)ts.tv_sec - last <= FLOW_MAX_AGE_SECONDS;
}

static int recent_forwarded(void)
{
    return recent_timestamp(&last_forwarded_seconds);
}

static int recent_worker_tick(void)
{
    return recent_timestamp(&last_worker_tick_seconds);
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

    if (!listener) return -1;
    gateway_health_listener_init(listener);
    if (trusted_directory(dir, health_uid()) != 0) return -1;
    n = snprintf(listener->path, sizeof(listener->path), "%s/gateway-health.sock", dir);
    if (n <= 0 || (size_t)n >= sizeof(listener->path)) return -1;
    if (unlink(listener->path) != 0 && errno != ENOENT) return -1;

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

int gateway_health_device_manager_ok(device_manager_t *devices)
{
    int ok;
    if (!devices || pthread_mutex_trylock(&devices->mutex) != 0) return 0;
    ok = devices->count <= DEVICE_MANAGER_MAX_DEVICES;
    pthread_mutex_unlock(&devices->mutex);
    return ok;
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
    manager_ok = gateway_health_device_manager_ok(devices);
    return queue_ok && manager_ok;
}

int gateway_health_check_pending_diag(const frame_data_t *data)
{
    const char *n1, *n2;
    char full_nonce[NONCE_LEN + 1];
    int match = 0;

    if (!data || strcmp(data->node_id, "_DIAG") != 0)
        return 0;
    n1 = frame_data_find_field(data, "N1");
    n2 = frame_data_find_field(data, "N2");
    if (!n1 || !n2 || strlen(n1) != 16 || strlen(n2) != 16)
        return 0;
    memcpy(full_nonce, n1, 16);
    memcpy(full_nonce + 16, n2, 16);
    full_nonce[NONCE_LEN] = '\0';

    pthread_mutex_lock(&local_diag_mutex);
    if (pending_local_nonce[0] &&
        memcmp(full_nonce, pending_local_nonce, NONCE_LEN) == 0) {
        match = 1;
    }
    pthread_mutex_unlock(&local_diag_mutex);
    return match;
}

int gateway_health_try_enqueue_local_marker(message_queue_t *queue,
                                            const char *json,
                                            size_t json_len)
{
    gateway_message_t msg;
    size_t pfx_len = sizeof(LOCAL_MARKER_PREFIX) - 1;
    int pushed = 0;

    if (!queue || !json || json_len == 0 ||
        pfx_len + json_len > GATEWAY_MESSAGE_MAX_SIZE)
        return 0;
    memcpy(msg.data, LOCAL_MARKER_PREFIX, pfx_len);
    memcpy(msg.data + pfx_len, json, json_len);
    msg.length = pfx_len + json_len;

    if (pthread_mutex_trylock(&queue->mutex) != 0)
        return 0;
    if (!queue->shutting_down && queue->count < MESSAGE_QUEUE_CAPACITY &&
        queue->head < MESSAGE_QUEUE_CAPACITY && queue->tail < MESSAGE_QUEUE_CAPACITY) {
        queue->items[queue->tail] = msg;
        queue->tail = (queue->tail + 1) % MESSAGE_QUEUE_CAPACITY;
        queue->count++;
        pthread_cond_signal(&queue->not_empty);
        pushed = 1;
    }
    pthread_mutex_unlock(&queue->mutex);
    return pushed;
}

int gateway_health_consume_local_marker(const unsigned char *data, size_t length)
{
    size_t pfx_len = sizeof(LOCAL_MARKER_PREFIX) - 1;
    frame_data_t decoded, roundtrip;
    char encoded_buf[GATEWAY_MESSAGE_MAX_SIZE];
    char full_nonce[NONCE_LEN + 1];
    int enc_len;
    const char *n1, *n2;

    if (!data || length <= pfx_len || length > GATEWAY_MESSAGE_MAX_SIZE) return 0;
    if (memcmp(data, LOCAL_MARKER_PREFIX, pfx_len) != 0) return 0;
    /* Run full business DATA decode -> encode -> decode roundtrip inside server_link_worker */
    if (message_json_decode_data((const char *)(data + pfx_len), length - pfx_len, &decoded) < 0)
        return 1; /* Consume malformed local marker without ever sending upstream */
    if (strcmp(decoded.node_id, "_DIAG") != 0)
        return 1;
    enc_len = message_json_build_data(encoded_buf, sizeof(encoded_buf), &decoded);
    if (enc_len <= 0 ||
        message_json_decode_data(encoded_buf, (size_t)enc_len, &roundtrip) < 0)
        return 1;
    n1 = frame_data_find_field(&roundtrip, "N1");
    n2 = frame_data_find_field(&roundtrip, "N2");
    if (!n1 || !n2 || strlen(n1) != 16 || strlen(n2) != 16)
        return 1;
    memcpy(full_nonce, n1, 16);
    memcpy(full_nonce + 16, n2, 16);
    full_nonce[NONCE_LEN] = '\0';

    pthread_mutex_lock(&local_diag_mutex);
    if (pending_local_nonce[0] &&
        memcmp(full_nonce, pending_local_nonce, NONCE_LEN) == 0) {
        memcpy(consumed_local_nonce, pending_local_nonce, NONCE_LEN + 1);
    }
    pthread_mutex_unlock(&local_diag_mutex);
    return 1;
}

int gateway_health_consume_queued_marker(message_queue_t *queue)
{
    gateway_message_t marker;
    size_t i, j, found = MESSAGE_QUEUE_CAPACITY;
    const size_t pfx_len = sizeof(LOCAL_MARKER_PREFIX) - 1;
    if (!queue || pthread_mutex_trylock(&queue->mutex) != 0) return 0;
    if (queue->count > MESSAGE_QUEUE_CAPACITY || queue->head >= MESSAGE_QUEUE_CAPACITY ||
        queue->tail >= MESSAGE_QUEUE_CAPACITY) {
        pthread_mutex_unlock(&queue->mutex);
        return 0;
    }
    for (i = 0; i < queue->count; ++i) {
        gateway_message_t *m = &queue->items[(queue->head + i) % MESSAGE_QUEUE_CAPACITY];
        if (m->length > pfx_len && m->length <= GATEWAY_MESSAGE_MAX_SIZE &&
            memcmp(m->data, LOCAL_MARKER_PREFIX, pfx_len) == 0) {
            marker = *m;
            found = i;
            break;
        }
    }
    if (found < MESSAGE_QUEUE_CAPACITY) {
        for (j = found; j + 1 < queue->count; ++j)
            queue->items[(queue->head + j) % MESSAGE_QUEUE_CAPACITY] =
                queue->items[(queue->head + j + 1) % MESSAGE_QUEUE_CAPACITY];
        queue->tail = (queue->tail + MESSAGE_QUEUE_CAPACITY - 1) % MESSAGE_QUEUE_CAPACITY;
        --queue->count;
        pthread_cond_signal(&queue->not_full);
    }
    pthread_mutex_unlock(&queue->mutex);
    if (found < MESSAGE_QUEUE_CAPACITY)
        return gateway_health_consume_local_marker(marker.data, marker.length);
    return 0;
}

/* Exercise real TCP loopback ingress -> gateway_loop accept() -> wifi_client_worker ->
 * line_parser_feed -> gateway_app_on_wifi_line -> upstream_queue -> server_link_worker. */
static int ingress_try_roundtrip_local_diag(uint16_t wifi_port, const char *nonce)
{
    char line[256];
    int n, fd = -1, ok = 0, err = 0;
    struct sockaddr_in addr;
    size_t sent = 0, total;
    long long deadline;

    if (wifi_port == 0 || !nonce || strlen(nonce) != NONCE_LEN)
        return 0;
    n = snprintf(line, sizeof(line),
                 "{\"node\":\"_DIAG\",\"seq\":1,\"type\":\"DATA\","
                 "\"fields\":{\"N1\":\"%.16s\",\"N2\":\"%.16s\"}}\n",
                 nonce, nonce + 16);
    if (n <= 0 || (size_t)n >= sizeof(line))
        return 0;
    total = (size_t)n;

    pthread_mutex_lock(&local_diag_mutex);
    memcpy(pending_local_nonce, nonce, NONCE_LEN + 1);
    consumed_local_nonce[0] = '\0';
    pthread_mutex_unlock(&local_diag_mutex);

    fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) goto out;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(wifi_port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    deadline = now_ms();
    if (deadline < 0) goto out;
    deadline += 150;

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        socklen_t elen = sizeof(err);
        long long now = now_ms();
        int r;
        if (errno != EINPROGRESS || now < 0 || now >= deadline) goto out;
        r = poll(&pfd, 1, (int)(deadline - now));
        if (r <= 0 || !(pfd.revents & POLLOUT) ||
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen) != 0 || err != 0)
            goto out;
    }

    while (sent < total) {
        struct pollfd pfd = {fd, POLLOUT, 0};
        long long now = now_ms();
        ssize_t w;
        int r;
        if (now < 0 || now >= deadline) goto out;
        r = poll(&pfd, 1, (int)(deadline - now));
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0 || !(pfd.revents & POLLOUT)) goto out;
        w = send(fd, line + sent, total - sent, MSG_NOSIGNAL);
        if (w < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (w <= 0) goto out;
        sent += (size_t)w;
    }
    close(fd);
    fd = -1;

    /* Bounded wait up to 450ms for full ingress -> gateway_app -> server_link consumption. */
    {
        long long end = now_ms();
        if (end >= 0) {
            end += 450;
            for (;;) {
                int done = 0;
                pthread_mutex_lock(&local_diag_mutex);
                done = (memcmp(consumed_local_nonce, nonce, NONCE_LEN) == 0);
                pthread_mutex_unlock(&local_diag_mutex);
                if (done) { ok = 1; break; }
                if (now_ms() >= end) break;
                {
                    struct timespec ts = {0, 5000000L}; /* 5 ms */
                    nanosleep(&ts, NULL);
                }
            }
        }
    }

out:
    if (fd >= 0) close(fd);
    pthread_mutex_lock(&local_diag_mutex);
    pending_local_nonce[0] = '\0';
    consumed_local_nonce[0] = '\0';
    pthread_mutex_unlock(&local_diag_mutex);
    return ok;
}

void gateway_health_listener_service(gateway_health_listener_t *listener,
                                     message_queue_t *queue,
                                     device_manager_t *devices,
                                     uint16_t wifi_port)
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
         memcmp(request, FLOW_PREFIX, sizeof(FLOW_PREFIX) - 1) != 0 &&
         memcmp(request, LOCAL_PREFIX, sizeof(LOCAL_PREFIX) - 1) != 0)) {
        close(client);
        return;
    }
    request[used - 1] = '\0';
    for (i = sizeof(QUERY_PREFIX) - 1; i < used - 1; ++i)
        if (!isxdigit((unsigned char)request[i])) { close(client); return; }

    {
        const char *nonce = request + sizeof(QUERY_PREFIX) - 1;
        int flow = memcmp(request, FLOW_PREFIX, sizeof(FLOW_PREFIX) - 1) == 0;
        int local = memcmp(request, LOCAL_PREFIX, sizeof(LOCAL_PREFIX) - 1) == 0;
        const char *prefix = flow ? FLOW_PREFIX : (local ? LOCAL_PREFIX : QUERY_PREFIX);
        const char *verdict;
        int n;
        if (flow) {
            verdict = recent_forwarded() ? "FORWARDED" : "UNKNOWN";
        } else if (local) {
            verdict = (business_components_ready(nonce, queue, devices) &&
                       recent_worker_tick() &&
                       ingress_try_roundtrip_local_diag(wifi_port, nonce)) ? "LOCAL_OK" : "STALLED";
        } else {
            verdict = business_components_ready(nonce, queue, devices) ? "READY" : "NOT_READY";
        }
        n = snprintf(response, sizeof(response), "%s%s %s\n", prefix, nonce, verdict);
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
