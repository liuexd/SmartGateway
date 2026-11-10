#define _GNU_SOURCE

#include "service_ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#define IPC_LINE_MAX 512

/* ------------------------------------------------------------------ */
/* 名称表                                                              */
/* ------------------------------------------------------------------ */

static const struct {
    service_ipc_request_t request;
    const char *name;
} request_names[] = {
    { SERVICE_IPC_STATUS, "STATUS" },
    { SERVICE_IPC_START,  "START"  },
    { SERVICE_IPC_STOP,   "STOP"   },
    { SERVICE_IPC_RELOAD, "RELOAD" },
};

#define REQUEST_COUNT \
    (sizeof(request_names) / sizeof(request_names[0]))

static const struct {
    service_ipc_result_t result;
    const char *name;
} result_names[] = {
    { SERVICE_IPC_OK,          "OK"           },
    { SERVICE_IPC_ALREADY,     "ALREADY"      },
    { SERVICE_IPC_NOT_RUNNING, "NOT_RUNNING"  },
    { SERVICE_IPC_BUSY,        "BUSY"         },
    { SERVICE_IPC_FAILED,      "FAILED"       },
    { SERVICE_IPC_BAD_REQUEST, "BAD_REQUEST"  },
};

#define RESULT_COUNT \
    (sizeof(result_names) / sizeof(result_names[0]))

const char *service_ipc_result_name(service_ipc_result_t result)
{
    for (size_t i = 0; i < RESULT_COUNT; i++) {
        if (result_names[i].result == result)
            return result_names[i].name;
    }

    return "UNKNOWN";
}

int service_ipc_result_parse(
    const char *name,
    service_ipc_result_t *out)
{
    if (name == NULL || out == NULL)
        return -1;

    for (size_t i = 0; i < RESULT_COUNT; i++) {
        if (strcmp(result_names[i].name, name) == 0) {
            *out = result_names[i].result;
            return 0;
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* 路径                                                                */
/* ------------------------------------------------------------------ */

int service_ipc_path(
    const char *run_dir,
    char *output,
    size_t capacity)
{
    if (run_dir == NULL || run_dir[0] == '\0' ||
        output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(
        output, capacity,
        "%s/%s", run_dir, SERVICE_IPC_SOCKET_NAME);

    /*
     * 必须检查截断。
     *
     * sun_path 只有 108 字节，若路径被悄悄截断，绑定的
     * 会是一个**完全不同**的路径，且不报错——排查起来极其困难。
     */
    if (n < 0 || (size_t)n >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 编码                                                                */
/* ------------------------------------------------------------------ */

int service_ipc_encode_request(
    service_ipc_request_t request,
    char *output,
    size_t capacity)
{
    const char *name = NULL;

    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        if (request_names[i].request == request) {
            name = request_names[i].name;
            break;
        }
    }

    if (name == NULL || output == NULL) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(output, capacity, "%s\n", name);

    if (n < 0 || (size_t)n >= capacity) {
        errno = ENOSPC;
        return -1;
    }

    return n;
}

/*
 * 去掉首尾空白（含 \r\n）。
 */
static void trim(char *s)
{
    size_t len = strlen(s);

    while (len > 0 &&
           (s[len - 1] == ' ' || s[len - 1] == '\t' ||
            s[len - 1] == '\r' || s[len - 1] == '\n')) {
        s[--len] = '\0';
    }

    size_t start = 0;

    while (s[start] == ' ' || s[start] == '\t')
        start++;

    if (start > 0)
        memmove(s, s + start, len - start + 1);
}

int service_ipc_decode_request(
    const char *line,
    service_ipc_request_t *out)
{
    if (line == NULL || out == NULL)
        return -1;

    char buffer[IPC_LINE_MAX];

    if (strlen(line) >= sizeof(buffer)) {
        errno = EMSGSIZE;
        return -1;
    }

    memcpy(buffer, line, strlen(line) + 1);
    trim(buffer);

    /*
     * 只取第一个 token 作为命令名。
     *
     * 带参数的 START 形如 "START 9000 127.0.0.1"，
     * 若拿整行去比对名字，会被当成未知命令。
     */
    char *saveptr = NULL;
    char *token = strtok_r(buffer, " ", &saveptr);

    if (token == NULL) {
        *out = SERVICE_IPC_UNKNOWN;
        errno = EINVAL;
        return -1;
    }

    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        if (strcmp(request_names[i].name, token) == 0) {
            *out = request_names[i].request;
            return 0;
        }
    }

    *out = SERVICE_IPC_UNKNOWN;
    errno = EINVAL;
    return -1;
}

int service_ipc_encode_call(
    const service_ipc_call_t *call,
    char *output,
    size_t capacity)
{
    if (call == NULL || output == NULL)
        return -1;

    /*
     * 仅 START 支持参数。其余请求直接走无参编码。
     */
    if (call->request != SERVICE_IPC_START ||
        (call->upstream_port == 0 &&
         call->upstream_server[0] == '\0')) {
        return service_ipc_encode_request(
            call->request, output, capacity);
    }

    /*
     * 部分参数必须拒绝。
     *
     * "只给端口不给服务器"在语义上是含糊的：
     * 应该沿用配置里的服务器吗？那样会得到一个
     * "端口来自请求、服务器来自配置"的混合状态，
     * 而调用方可能以为服务器也换了。
     */
    if (call->upstream_port == 0 ||
        call->upstream_server[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    if (call->upstream_port < 0 || call->upstream_port > 65535) {
        errno = EINVAL;
        return -1;
    }

    /*
     * 服务器地址不能含空白，否则会破坏空格分隔的格式。
     * 这不是理论问题：一个空字符串或拼写错误带空格的值
     * 会让整个请求行被解析成完全不同的东西。
     */
    for (const char *p = call->upstream_server; *p != '\0'; p++) {
        if (*p == ' ' || *p == '\t' ||
            *p == '\r' || *p == '\n') {
            errno = EINVAL;
            return -1;
        }
    }

    const char *name = NULL;

    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        if (request_names[i].request == call->request) {
            name = request_names[i].name;
            break;
        }
    }

    if (name == NULL) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(
        output, capacity,
        "%s %d %s\n",
        name, call->upstream_port, call->upstream_server);

    if (n < 0 || (size_t)n >= capacity) {
        errno = ENOSPC;
        return -1;
    }

    return n;
}

int service_ipc_decode_call(
    const char *line,
    service_ipc_call_t *out)
{
    if (line == NULL || out == NULL)
        return -1;

    memset(out, 0, sizeof(*out));

    char buffer[IPC_LINE_MAX];

    if (strlen(line) >= sizeof(buffer)) {
        errno = EMSGSIZE;
        return -1;
    }

    memcpy(buffer, line, strlen(line) + 1);
    trim(buffer);

    char *saveptr = NULL;
    char *token = strtok_r(buffer, " ", &saveptr);

    if (token == NULL)
        return -1;

    int matched = 0;

    for (size_t i = 0; i < REQUEST_COUNT; i++) {
        if (strcmp(request_names[i].name, token) == 0) {
            out->request = request_names[i].request;
            matched = 1;
            break;
        }
    }

    if (!matched) {
        out->request = SERVICE_IPC_UNKNOWN;
        errno = EINVAL;
        return -1;
    }

    /* 无参数：正常情况 */
    token = strtok_r(NULL, " ", &saveptr);

    if (token == NULL)
        return 0;

    /* 只有 START 允许带参数 */
    if (out->request != SERVICE_IPC_START) {
        errno = EINVAL;
        return -1;
    }

    char *end = NULL;

    errno = 0;
    long port = strtol(token, &end, 10);

    if (errno != 0 || end == token || *end != '\0' ||
        port <= 0 || port > 65535) {
        errno = EINVAL;
        return -1;
    }

    out->upstream_port = (int)port;

    token = strtok_r(NULL, " ", &saveptr);

    if (token == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (strlen(token) >= sizeof(out->upstream_server)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(out->upstream_server, token, strlen(token) + 1);

    return 0;
}

/* ------------------------------------------------------------------ */
/* 响应                                                                */
/* ------------------------------------------------------------------ */

int service_ipc_encode_reply(
    const service_ipc_reply_t *reply,
    char *output,
    size_t capacity)
{
    if (reply == NULL || output == NULL)
        return -1;

    /*
     * detail 必须压成单行：一旦含换行，客户端按行读取会
     * 把后半截当成下一条响应的开头，协议就散了。
     */
    char detail[SERVICE_IPC_MESSAGE_MAX];

    snprintf(detail, sizeof(detail), "%s", reply->detail);

    for (char *p = detail; *p != '\0'; p++) {
        if (*p == '\n' || *p == '\r')
            *p = ' ';
    }

    int n = snprintf(
        output, capacity,
        "%s %ld %llu %s\n",
        service_ipc_result_name(reply->result),
        (long)reply->pid,
        reply->starttime,
        detail);

    if (n < 0 || (size_t)n >= capacity) {
        errno = ENOSPC;
        return -1;
    }

    return n;
}

/*
 * 解析无符号 64 位。
 *
 * 不用 sscanf("%llu")：溢出时 glibc 返回 ULLONG_MAX 且仍报成功，
 * 一个被破坏的超大数字会被当成合法值接受。
 * （该缺陷在 OTA 的 pidfile 解析中已实测确认过一次。）
 */
static int parse_u64(const char *text, unsigned long long *out)
{
    if (text == NULL || *text == '\0' || out == NULL)
        return -1;

    char *end = NULL;

    errno = 0;
    unsigned long long value = strtoull(text, &end, 10);

    if (errno != 0 || end == text || *end != '\0')
        return -1;

    *out = value;
    return 0;
}

int service_ipc_decode_reply(
    const char *line,
    service_ipc_reply_t *out)
{
    if (line == NULL || out == NULL)
        return -1;

    char buffer[IPC_LINE_MAX];

    if (strlen(line) >= sizeof(buffer)) {
        errno = EMSGSIZE;
        return -1;
    }

    memcpy(buffer, line, strlen(line) + 1);
    trim(buffer);

    memset(out, 0, sizeof(*out));

    /* 拆出 result 字段 */
    char *saveptr = NULL;
    char *token = strtok_r(buffer, " ", &saveptr);

    if (token == NULL ||
        service_ipc_result_parse(token, &out->result) != 0) {
        errno = EINVAL;
        return -1;
    }

    /* pid */
    token = strtok_r(NULL, " ", &saveptr);

    if (token == NULL) {
        errno = EINVAL;
        return -1;
    }

    char *end = NULL;

    errno = 0;
    long pid_value = strtol(token, &end, 10);

    if (errno != 0 || end == token || *end != '\0' || pid_value < 0) {
        errno = EINVAL;
        return -1;
    }

    out->pid = (pid_t)pid_value;

    /* starttime */
    token = strtok_r(NULL, " ", &saveptr);

    if (token == NULL ||
        parse_u64(token, &out->starttime) != 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * 剩余部分是 detail（可能含空格，也可能为空）。
     * 用 strtok_r 会把它再切开，所以直接取原串中该位置之后的内容。
     */
    token = strtok_r(NULL, "", &saveptr);

    if (token != NULL) {
        snprintf(out->detail, sizeof(out->detail), "%s", token);
        trim(out->detail);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 客户端                                                              */
/* ------------------------------------------------------------------ */

static int connect_socket(const char *path, int timeout_ms)
{
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        (void)close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(addr.sun_path, path, strlen(path) + 1);

    /*
     * 非阻塞连接 + poll 超时。
     *
     * 直接阻塞 connect 到 Unix socket 通常很快，但若 supervisor
     * 卡住（例如正在等 Gateway 退出），accept 队列满后仍可能
     * 长时间阻塞。有超时才能保证 OTA 不会被拖死。
     */
    int flags = fcntl(fd, F_GETFL, 0);

    if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        (void)close(fd);
        return -1;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        if (errno != EINPROGRESS) {
            int saved = errno;
            (void)close(fd);
            errno = saved;
            return -1;
        }

        struct pollfd pfd = {
            .fd = fd,
            .events = POLLOUT,
            .revents = 0,
        };

        int ready = poll(&pfd, 1, timeout_ms);

        if (ready <= 0) {
            int saved = ready == 0 ? ETIMEDOUT : errno;
            (void)close(fd);
            errno = saved;
            return -1;
        }

        /* 检查连接是否真的建立 */
        int so_error = 0;
        socklen_t len = sizeof(so_error);

        if (getsockopt(
                fd, SOL_SOCKET, SO_ERROR,
                &so_error, &len) != 0 ||
            so_error != 0) {
            int saved = so_error != 0 ? so_error : errno;
            (void)close(fd);
            errno = saved;
            return -1;
        }
    }

    /* 恢复阻塞模式，后续读写用 poll 控超时 */
    if (fcntl(fd, F_SETFL, flags) < 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }

    return fd;
}

int service_ipc_call(
    const char *run_dir,
    service_ipc_request_t request,
    int timeout_ms,
    service_ipc_reply_t *reply_out)
{
    service_ipc_call_t call = { .request = request };

    return service_ipc_call_ex(run_dir, &call, timeout_ms, reply_out);
}

int service_ipc_call_ex(
    const char *run_dir,
    const service_ipc_call_t *call,
    int timeout_ms,
    service_ipc_reply_t *reply_out)
{
    char path[128];

    if (call == NULL)
        return -1;

    if (service_ipc_path(
            run_dir, path, sizeof(path)) != 0)
        return -1;

    char request_line[128];

    if (service_ipc_encode_call(
            call, request_line,
            sizeof(request_line)) < 0)
        return -1;

    int fd = connect_socket(path, timeout_ms);

    if (fd < 0)
        return -1;

    /* 发送 */
    size_t sent = 0;
    size_t total = strlen(request_line);

    while (sent < total) {
        ssize_t n = send(
            fd, request_line + sent, total - sent, MSG_NOSIGNAL);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            int saved = errno;
            (void)close(fd);
            errno = saved ? saved : EIO;
            return -1;
        }

        sent += (size_t)n;
    }

    /* 接收一行 */
    char line[IPC_LINE_MAX];
    size_t used = 0;
    int saved_errno = ETIMEDOUT;

    for (;;) {
        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN,
            .revents = 0,
        };

        int ready = poll(&pfd, 1, timeout_ms);

        if (ready == 0)
            break;      /* 超时 */

        if (ready < 0) {
            if (errno == EINTR)
                continue;

            saved_errno = errno;
            break;
        }

        if (used + 1 >= sizeof(line)) {
            saved_errno = EMSGSIZE;
            break;
        }

        ssize_t n = recv(
            fd, line + used,
            sizeof(line) - used - 1, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            saved_errno = errno;
            break;
        }

        if (n == 0)
            break;      /* 对端关闭 */

        used += (size_t)n;
        line[used] = '\0';

        if (strchr(line, '\n') != NULL) {
            saved_errno = 0;
            break;
        }
    }

    (void)close(fd);

    if (saved_errno != 0) {
        errno = saved_errno;
        return -1;
    }

    if (reply_out != NULL &&
        service_ipc_decode_reply(line, reply_out) != 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/* 服务端                                                              */
/* ------------------------------------------------------------------ */

int service_ipc_listen(
    const char *run_dir,
    char *path_out,
    size_t path_capacity)
{
    char path[128];

    if (service_ipc_path(
            run_dir, path, sizeof(path)) != 0)
        return -1;

    if (path_out != NULL && path_capacity > 0) {
        if (strlen(path) >= path_capacity) {
            errno = ENAMETOOLONG;
            return -1;
        }

        memcpy(path_out, path, strlen(path) + 1);
    }

    /*
     * 已有 socket 文件时先探测：
     *
     *   能连上  -> 另一个 supervisor 正在跑（EADDRINUSE）
     *   连不上  -> 上次崩溃留下的陈旧文件，删掉重建
     *
     * 直接 unlink 再 bind 是危险的：会让两个 supervisor 同时
     * 认为自己是唯一的，于是双方都去启动 Gateway。
     */
    struct stat st;

    if (lstat(path, &st) == 0) {
        if (!S_ISSOCK(st.st_mode)) {
            errno = EEXIST;
            return -1;
        }

        int probe = connect_socket(path, 500);

        if (probe >= 0) {
            (void)close(probe);
            errno = EADDRINUSE;
            return -1;
        }

        /* 无人监听：陈旧文件 */
        if (unlink(path) != 0 && errno != ENOENT)
            return -1;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;

    if (strlen(path) >= sizeof(addr.sun_path)) {
        (void)close(fd);
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(addr.sun_path, path, strlen(path) + 1);

    /*
     * 权限 0600：只有 root 能连接。
     *
     * umask 在这里不起作用——bind 创建的 socket 权限是
     * 0777 & ~umask，用显式 chmod 才能保证结果确定。
     */
    mode_t old_mask = umask(0177);

    int rc = bind(fd, (struct sockaddr *)&addr, sizeof(addr));

    umask(old_mask);

    if (rc != 0) {
        int saved = errno;
        (void)close(fd);
        errno = saved;
        return -1;
    }

    if (chmod(path, 0600) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved;
        return -1;
    }

    if (listen(fd, 8) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(path);
        errno = saved;
        return -1;
    }

    return fd;
}

int service_ipc_accept_call(
    int listen_fd,
    service_ipc_call_t *call_out)
{
    if (call_out == NULL)
        return -1;

    int client = accept4(
        listen_fd, NULL, NULL, SOCK_CLOEXEC);

    if (client < 0)
        return -1;

    /*
     * 读取一行请求。
     *
     * 设 SO_RCVTIMEO 而不是依赖对端自觉：一个只连不发（或
     * 发一半就挂住）的客户端会让 supervisor 永远卡在这里，
     * 整个服务管理就此失去响应。
     */
    struct timeval tv = { .tv_sec = 2, .tv_usec = 0 };

    (void)setsockopt(
        client, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    char line[IPC_LINE_MAX];
    size_t used = 0;

    for (;;) {
        if (used + 1 >= sizeof(line)) {
            (void)close(client);
            errno = EMSGSIZE;
            return -1;
        }

        ssize_t n = recv(
            client, line + used,
            sizeof(line) - used - 1, 0);

        if (n < 0) {
            if (errno == EINTR)
                continue;

            (void)close(client);
            return -1;
        }

        if (n == 0)
            break;

        used += (size_t)n;
        line[used] = '\0';

        if (strchr(line, '\n') != NULL)
            break;
    }

    if (used == 0) {
        (void)close(client);
        errno = EPROTO;
        return -1;
    }

    if (service_ipc_decode_call(line, call_out) != 0) {
        /*
         * 请求无法解析时不直接关闭，而是交给调用方回复
         * BAD_REQUEST —— 让客户端知道是自己发错了，
         * 而不是 supervisor 挂了。
         */
        call_out->request = SERVICE_IPC_UNKNOWN;
    }

    return client;
}

int service_ipc_accept_request(
    int listen_fd,
    service_ipc_request_t *request_out)
{
    if (request_out == NULL)
        return -1;

    service_ipc_call_t call;

    int client = service_ipc_accept_call(listen_fd, &call);

    if (client < 0)
        return -1;

    *request_out = call.request;
    return client;
}

int service_ipc_send_reply_and_close(
    int client_fd,
    const service_ipc_reply_t *reply)
{
    char line[SERVICE_IPC_MESSAGE_MAX + 128];

    int n = service_ipc_encode_reply(
        reply, line, sizeof(line));

    if (n < 0) {
        (void)close(client_fd);
        return -1;
    }

    size_t sent = 0;
    size_t total = (size_t)n;

    while (sent < total) {
        ssize_t w = send(
            client_fd, line + sent, total - sent, MSG_NOSIGNAL);

        if (w < 0 && errno == EINTR)
            continue;

        if (w <= 0) {
            int saved = errno;
            (void)close(client_fd);
            errno = saved ? saved : EIO;
            return -1;
        }

        sent += (size_t)w;
    }

    (void)close(client_fd);
    return 0;
}
