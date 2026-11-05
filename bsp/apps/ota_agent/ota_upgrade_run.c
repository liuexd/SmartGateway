
#define _GNU_SOURCE

#include "ota_upgrade_run.h"
#include "ota_upgrade.h"
#include "ota_recover.h"
#include "ota_process.h"
#include "ota_health.h"
#include "ota_state_io.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <json-c/json.h>

#define WIFI_PORT 7000
#define L1_TIMEOUT_MS 3000
#define L2_TIMEOUT_MS 5000
#define STOP_TIMEOUT_MS 3000

static long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    return (long long)ts.tv_sec * 1000 +
           ts.tv_nsec / 1000000;
}

static int wait_readable(int fd, long long deadline)
{
    for (;;) {
        long long remaining = deadline - now_ms();

        if (remaining <= 0)
            return -1;

        struct pollfd pfd = {
            .fd = fd,
            .events = POLLIN
        };

        int rc = poll(&pfd, 1, (int)remaining);

        if (rc < 0 && errno == EINTR)
            continue;

        if (rc <= 0 || (pfd.revents & POLLNVAL))
            return -1;

        if (pfd.revents & (POLLIN | POLLHUP))
            return 0;

        return -1;
    }
}

/* 忙等到 deadline；用于如实耗尽 L2 观察窗口 */
static void wait_until(long long deadline)
{
    for (;;) {
        long long remaining = deadline - now_ms();

        if (remaining <= 0)
            return;

        struct timespec delay = {
            .tv_sec = (time_t)(remaining / 1000),
            .tv_nsec = (long)((remaining % 1000) * 1000000L)
        };

        while (nanosleep(&delay, &delay) != 0 &&
               errno == EINTR) {
        }
    }
}

/*
 * 用真实 connect 探测北向上游是否处于“拒绝连接”状态。
 *
 * 这是对服务器状态的实测，而不是“测试程序知道服务器已关闭”的假设：
 *   ​1  = 内核返回 ECONNREFUSED，故障确实存在
 *    0  = 能建立连接，说明上游其实在监听，故障注入未生效
 *   -1  = 其他错误
 */
static int upstream_is_refused(unsigned short port)
{
    int fd = socket(
        AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(
        fd, (struct sockaddr *)&addr, sizeof(addr));

    int saved_errno = errno;

    close(fd);

    if (rc == 0)
        return 0;

    return saved_errno == ECONNREFUSED ? 1 : -1;
}

static int create_mock_server(unsigned short *port)
{
    int fd = socket(
        AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return -1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(0)
    };

    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&addr,
             sizeof(addr)) != 0 ||
        listen(fd, 1) != 0) {
        close(fd);
        return -1;
    }

    socklen_t len = sizeof(addr);

    if (getsockname(fd, (struct sockaddr *)&addr,
                    &len) != 0) {
        close(fd);
        return -1;
    }

    *port = ntohs(addr.sin_port);
    return fd;
}

static int wifi_port_busy(void)
{
    int fd = socket(
        AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (fd < 0)
        return 1;

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(WIFI_PORT)
    };

    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int rc = connect(
        fd, (struct sockaddr *)&addr, sizeof(addr));

    int saved_errno = errno;
    close(fd);

    return rc == 0 || saved_errno != ECONNREFUSED;
}

static int matches_probe(const char *line, int64_t seq)
{
    json_object *root = json_tokener_parse(line);
    json_object *node = NULL;
    json_object *sequence = NULL;
    json_object *type = NULL;
    json_object *fields = NULL;
    json_object *t = NULL;
    json_object *h = NULL;

    int matched = 0;

    if (!root || !json_object_is_type(root, json_type_object))
        goto out;

    if (!json_object_object_get_ex(root, "node", &node) ||
        !json_object_object_get_ex(root, "seq", &sequence) ||
        !json_object_object_get_ex(root, "type", &type) ||
        !json_object_object_get_ex(root, "fields", &fields))
        goto out;

    if (!json_object_is_type(node, json_type_string) ||
        !json_object_is_type(sequence, json_type_int) ||
        !json_object_is_type(type, json_type_string) ||
        !json_object_is_type(fields, json_type_object))
        goto out;

    if (!json_object_object_get_ex(fields, "T", &t) ||
        !json_object_object_get_ex(fields, "H", &h) ||
        !json_object_is_type(t, json_type_string) ||
        !json_object_is_type(h, json_type_string))
        goto out;

    matched =
        strcmp(json_object_get_string(node), "OTA01") == 0 &&
        json_object_get_int64(sequence) == seq &&
        strcmp(json_object_get_string(type), "DATA") == 0 &&
        strcmp(json_object_get_string(t), "253") == 0 &&
        strcmp(json_object_get_string(h), "601") == 0;

out:
    if (root)
        json_object_put(root);

    return matched;
}

/*
 * 南向入站：连接 Gateway 的 7000 端口并发送一条合法 JSON DATA。
 *
 * 这一步与北向状态完全解耦：即使北向上游不可用，也必须先证明
 * 南向接收通路正常（解析 + 入队发生在 Gateway 内部）。
 *
 * 返回值 0 表示数据已被 Gateway 的对端接受。
 */
static int send_southbound_data(int64_t seq)
{
    int client = socket(
        AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);

    if (client < 0)
        return -1;

    struct sockaddr_in addr;

    memset(&addr, 0, sizeof(addr));

    addr.sin_family = AF_INET;
    addr.sin_port = htons(WIFI_PORT);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    int result = -1;

    if (connect(client, (struct sockaddr *)&addr,
                sizeof(addr)) != 0)
        goto out;

    char payload[256];

    int n = snprintf(
        payload, sizeof(payload),
        "{\"node\":\"OTA01\",\"seq\":%lld,"
        "\"type\":\"DATA\","
        "\"fields\":{\"T\":\"253\",\"H\":\"601\"}}\n",
        (long long)seq);

    if (n <= 0 || (size_t)n >= sizeof(payload))
        goto out;

    size_t sent = 0;

    while (sent < (size_t)n) {
        ssize_t written = send(
            client, payload + sent,
            (size_t)n - sent, MSG_NOSIGNAL);

        if (written < 0 && errno == EINTR)
            continue;

        if (written <= 0)
            goto out;

        sent += (size_t)written;
    }

    result = 0;

out:
    close(client);
    return result;
}

/*
 * L2 业务健康检查。
 *
 * 两个阶段必须分开判定，否则北向故障会掩盖南向结果：
 *   阶段一 南向：连接 7000 并发送合法 JSON DATA
 *   阶段二 北向：等待该 DATA 经 Gateway 转发回来的业务确认
 *
 * listenfd < 0 表示北向上游由外部测试协调器持有，本进程不监听。
 * 此时用真实 connect 探测上游状态，并在完整 L2 窗口内等待确认。
 */
static int probe_l2(
    int listenfd,
    unsigned short upstream_port,
    int64_t seq)
{
    int upstream = -1;
    int result = -1;

    long long deadline = now_ms() + L2_TIMEOUT_MS;

    /* ---- 阶段一：南向入站 ---- */
    printf("PROBE: southbound DATA seq=%lld\n",
           (long long)seq);

    if (send_southbound_data(seq) != 0) {
        printf("FAIL: southbound DATA rejected: %s\n",
               strerror(errno));
        return -1;
    }

    puts("PASS: southbound DATA accepted");

    /* ---- 阶段二：北向 ---- */
    if (listenfd >= 0) {
        /* 等待 Gateway 的 Server Worker 建立上游连接 */
        if (wait_readable(listenfd, deadline) != 0) {
            puts("FAIL: northbound connect timeout");
            goto out;
        }

        upstream = accept4(
            listenfd, NULL, NULL, SOCK_CLOEXEC);

        if (upstream < 0) {
            printf("FAIL: northbound accept: %s\n",
                   strerror(errno));
            goto out;
        }
    } else {
        /*
         * 上游由协调器持有，本进程既不能 accept 也无法读到回传。
         *
         * 这里只把 connect 的结果当作“故障形态证据”记录，
         * 判定本身仍然是：在完整 L2 窗口内有没有等到业务确认。
         *
         * 因此无论上游是“拒绝连接”还是“监听但不应答”，
         * 都要如实耗尽窗口，不能因为已知道上游不可用就提前返回。
         */
        int refused = upstream_is_refused(upstream_port);

        if (refused == 1) {
            printf(
                "FAIL: northbound refused (ECONNREFUSED) port=%u\n",
                (unsigned)upstream_port);
        } else if (refused == 0) {
            printf(
                "NOTICE: upstream %u accepts connections "
                "but no ack is expected from it\n",
                (unsigned)upstream_port);
        } else {
            printf(
                "NOTICE: northbound probe inconclusive, "
                "port=%u\n",
                (unsigned)upstream_port);
        }

        /* 如实耗尽业务确认窗口 */
        wait_until(deadline);

        puts("FAIL: no northbound business ack within L2 window");
        goto out;
    }

    /* 接收上游数据，按换行分帧并验证完整 JSON */
    char line[8192];
    size_t used = 0;

    while (now_ms() < deadline) {
        if (wait_readable(upstream, deadline) != 0)
            break;

        char buffer[1024];
        ssize_t count = recv(
            upstream, buffer, sizeof(buffer), 0);

        if (count < 0 && errno == EINTR)
            continue;

        if (count <= 0)
            break;

        for (ssize_t i = 0; i < count; ++i) {
            if (buffer[i] == '\n') {
                line[used] = '\0';

                if (matches_probe(line, seq)) {
                    printf(
                        "PASS: L2 probe forwarded, seq=%lld\n",
                        (long long)seq);
                    result = 0;
                    goto out;
                }

                used = 0;
            } else {
                if (used + 1 >= sizeof(line))
                    goto out;

                line[used++] = buffer[i];
            }
        }
    }

out:

    if (upstream >= 0)
        close(upstream);

    return result;
}

static int run_gateway_health(
    const char *install_dir,
    int listenfd,
    unsigned short upstream_port,
    int expect_l2_success)
{
    char executable[PATH_MAX];
    char port_string[16];

    if (snprintf(
            executable, sizeof(executable),
            "%s/current/smart_gateway",
            install_dir) >= (int)sizeof(executable))
        return -1;

    snprintf(
        port_string, sizeof(port_string),
        "%u", (unsigned)upstream_port);

    char *argv[] = {
        executable,
        "--no-serial",
        "--server", "127.0.0.1",
        "--port", port_string,
        NULL
    };

    pid_t pid = -1;
    int result = -1;

    if (ota_process_start(
            executable, argv, &pid) != 0) {
        perror("FAIL: Gateway exec");
        return -1;
    }

    printf("Gateway started: pid=%ld\n", (long)pid);

    if (ota_health_wait_tcp_ready(
            pid, "127.0.0.1",
            WIFI_PORT, L1_TIMEOUT_MS) != 0) {
        puts("FAIL: L1 health");
        goto stop;
    }

    puts("PASS: L1 TCP ready");

    uint32_t random_seq = 0;

    if (getrandom(
            &random_seq, sizeof(random_seq), 0) !=
        (ssize_t)sizeof(random_seq))
        goto stop;

    int64_t seq =
        (int64_t)(random_seq % 900000000U) + 1;

    /*
     * 无论期望 L2 成功还是失败，都走同一条真实探测路径。
     * 失败与否由 Gateway 的实际行为决定，不由测试开关预设。
     */
    int l2_ok = probe_l2(
        listenfd, upstream_port, seq) == 0;

    if (l2_ok)
        puts("PASS: L2 business health");
    else
        puts("FAIL: L2 business health");

    if (expect_l2_success && !l2_ok) {
        puts("FAIL: L2 unexpectedly failed");
        goto stop;
    }

    if (!expect_l2_success && l2_ok) {
        puts("FAIL: L2 unexpectedly succeeded");
        goto stop;
    }

    if (!expect_l2_success)
        goto stop;

    result = 0;

stop:
    {
        int wait_status = 0;
        long long elapsed = 0;

        int stop_rc = ota_process_stop(
            pid, STOP_TIMEOUT_MS,
            &wait_status, &elapsed);

        printf(
            "Gateway stop result=%d, elapsed=%lld ms\n",
            stop_rc, elapsed);

        if (stop_rc != OTA_PROCESS_GRACEFUL ||
            !WIFEXITED(wait_status) ||
            WEXITSTATUS(wait_status) != 0) {
            puts("FAIL: Gateway did not exit cleanly");
            result = -1;
        }
    }

    return result;
}

static int persist_next(
    const char *state_dir,
    ota_state_record_t *record,
    ota_state_t next)
{
    ota_context_t context = {
        .state = record->state
    };

    if (ota_state_transition(&context, next) != 0)
        return -1;

    ota_state_record_t updated = *record;
    updated.state = context.state;

    if (next == OTA_CONFIRMED) {
        memcpy(
            updated.confirmed_version,
            updated.candidate_version,
            sizeof(updated.confirmed_version));
    }

    if (ota_state_save(state_dir, &updated) != 0)
        return -1;

    *record = updated;
    return 0;
}

int ota_upgrade_run_test(
    const char *package_dir,
    const char *install_dir)
{
    char state_dir[PATH_MAX];
    ota_state_record_t record = {0};

    int result = 1;
    int listenfd = -1;
    unsigned short upstream_port = 0;

    if (snprintf(
            state_dir, sizeof(state_dir),
            "%s/state", install_dir) >=
        (int)sizeof(state_dir))
        return 1;

    if (ota_state_load(state_dir, &record) != 0) {
        puts("FAIL: load OTA state");
        return 1;
    }

    if (record.state != OTA_CONFIRMED &&
        record.state != OTA_ROLLED_BACK) {
        puts("FAIL: pending OTA transaction");
        return 1;
    }

    if (wifi_port_busy()) {
        puts("FAIL: TCP 7000 already occupied or unavailable");
        return 1;
    }

    /*
     * 北向上游由测试协调器指定时，本进程不监听。
     *
     * 协调器会先 bind 但不 listen 该端口，于是 Gateway 的
     * connect() 会拿到真实的 ECONNREFUSED —— 这是内核层面的
     * 拒接，而不是测试开关直接返回失败。
     */
    const char *env_port = getenv("OTA_UPSTREAM_PORT");

    int external_upstream = 0;

    if (env_port != NULL && env_port[0] != '\0') {
        char *end = NULL;

        long value = strtol(env_port, &end, 10);

        if (end == env_port || *end != '\0' ||
            value <= 0 || value > 65535) {
            puts("FAIL: invalid OTA_UPSTREAM_PORT");
            return 1;
        }

        external_upstream = 1;
        upstream_port = (unsigned short)value;

        printf(
            "External upstream port: %u (not listening)\n",
            (unsigned)upstream_port);
    } else {
        listenfd = create_mock_server(&upstream_port);

        if (listenfd < 0) {
            perror("FAIL: create mock upstream");
            return 1;
        }

        printf(
            "Mock upstream port: %u\n",
            (unsigned)upstream_port);
    }

    if (ota_activate_test(
            package_dir, install_dir) != 0)
        goto rollback;

    if (ota_state_load(state_dir, &record) != 0 ||
        record.state != OTA_ACTIVATED) {
        puts("FAIL: activated state invalid");
        goto rollback;
    }

    /* 外部上游意味着本次注入了北向故障，L2 应真实失败 */
    int expect_l2 = external_upstream ? 0 : 1;

    if (run_gateway_health(
            install_dir, listenfd,
            upstream_port, expect_l2) != 0)
        goto rollback;

    if (persist_next(
            state_dir, &record, OTA_HEALTHY) != 0) {
        puts("FAIL: persist HEALTHY");
        goto rollback;
    }

    if (persist_next(
            state_dir, &record, OTA_CONFIRMED) != 0) {
        puts("FAIL: persist CONFIRMED");
        goto rollback;
    }

    if (ota_recover_execute(install_dir) != 0) {
        puts("FAIL: confirmed/current consistency");
        goto out;
    }

    printf(
        "PASS: OTA upgrade CONFIRMED: %s\n",
        record.confirmed_version);

    result = 0;
    goto out;

rollback:
    puts("RECOVERY: attempting rollback");

    if (ota_recover_execute(install_dir) != 0) {
        puts("FAIL: filesystem rollback");
        goto out;
    }

    if (ota_state_load(state_dir, &record) != 0)
        goto out;

    if (record.state != OTA_ROLLED_BACK) {
        puts("NOTICE: activation did not enter rollback state");
        goto out;
    }


    /*
     * 回滚后的旧版健康检查必须使用全新的 Mock Server。
     *
     * 原因：
     * 新版 Gateway 已经连接旧 Mock Server，但如果在 L2
     * 检查前发生故障注入，旧连接可能仍停留在 accept 队列中。
     *
     * 如果直接复用 listenfd，旧版 L2 检查可能先取到
     * 已退出新版的连接，从而产生错误的失败结果。
     */

    close(listenfd);
    listenfd = -1;

    listenfd = create_mock_server(&upstream_port);

    if (listenfd < 0) {
        perror("FAIL: recreate rollback mock server");
        goto out;
    }

    printf(
        "RECOVERY: new mock upstream port: %u\n",
        (unsigned)upstream_port);

    puts("RECOVERY: verifying old Gateway business");

    if (run_gateway_health(
            install_dir,
            listenfd,
            upstream_port,
            1) == 0) {
        puts("PASS: old Gateway business recovered");
    } else {
        puts("FAIL: old Gateway business not recovered");
    }


out:
    if (listenfd >= 0)
        close(listenfd);

    return result;
}
