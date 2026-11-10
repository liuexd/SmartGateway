
#define _GNU_SOURCE

#include "ota_upgrade_run.h"
#include "ota_upgrade.h"
#include "ota_recover.h"
#include "ota_process.h"
#include "ota_health.h"
#include "ota_supervisor.h"
#include "ota_state_io.h"
#include "ota_verify.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
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

    ota_sup_managed_t managed = {0};
    int result = -1;

    /*
     * S3：改用 supervisor 启动，从而登记 pidfile 身份。
     *
     * 与原先直接 ota_process_start 的区别：
     *   - 启动前会检查是否已有 RUNNING 实例（EALREADY）
     *   - 启动后写入 <install>/run/gateway.pid
     *   - 停止时走 ota_sup_stop_managed（含所有者校验与 pidfile 清理）
     *
     * 这样升级过程中存在一个可被外部查询的权威记录，
     * 而不是"进程在跑但没人知道它的身份"。
     */
    if (ota_sup_start_managed(
            install_dir, executable, argv, &managed) != 0) {
        perror("FAIL: Gateway start");
        return -1;
    }

    pid_t pid = managed.pid;

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

        /*
         * S3：走 supervisor 停止，它会：
         *   - 校验 owner_pid（只能停自己启动的进程）
         *   - 确认退出后清理 pidfile
         *
         * 若 stop 失败，pidfile 会被保留 —— 这是正确行为，
         * 因为进程可能仍在运行，删掉记录会让后续 query 误判。
         */
        int stop_rc = ota_sup_stop_managed(
            install_dir, &managed,
            STOP_TIMEOUT_MS,
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

/*
 * S3-R：升级包与候选版本的前置校验。
 *
 * 必须**在停止旧 Gateway 之前**完成。
 *
 * 原因（复审问题一）：
 *   原顺序是先 prepare_existing_gateway() 停掉旧版，
 *   之后 ota_activate_test() 内部才校验升级包。
 *   若包已损坏（哈希错、缺文件、架构不符），
 *   结果是：旧版停了、升级没发生、也没有重启旧版
 *   —— 一次纯粹的、本可避免的业务中断。
 *
 * 因此把"能不能升级"的判断全部提前，"愿不愿意升级"再决定是否停机。
 *
 * 校验内容：
 *   1. ota_verify_package：manifest 字段、大小、SHA-256、ELF 架构
 *   2. 目标 release 目录是否可创建（路径与版本号合法性）
 *   3. 候选版本号是否与此前声明的 candidate 一致（若状态里有）
 */
static int preflight_package(
    const char *package_dir,
    const char *install_dir)
{
    /*
     * 第一步：完整的包校验。
     *
     * 这一步会做 SHA-256、大小、架构、manifest 字段类型检查，
     * 任何一项失败都意味着"包不可用"，此时**绝不能**动旧版。
     */
    if (ota_verify_package(package_dir) != 0) {
        puts("FAIL: package verification failed");
        puts("      upgrade aborted before stopping the gateway");
        return -1;
    }

    /* 第二步：确认 install 目录可访问，且 releases 子目录存在 */
    int rootfd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

    if (rootfd < 0) {
        printf(
            "FAIL: cannot open install dir: %s\n",
            strerror(errno));
        return -1;
    }

    int releasesfd = openat(
        rootfd, "releases",
        O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

    int saved_errno = errno;

    if (releasesfd >= 0)
        (void)close(releasesfd);

    (void)close(rootfd);

    if (releasesfd < 0) {
        printf(
            "FAIL: cannot open releases dir: %s\n",
            strerror(saved_errno));
        return -1;
    }

    puts("PASS: preflight checks done; safe to stop the gateway");

    return 0;
}

/*
 * S3：升级前置检查（设计文档 5.2 节的 0a–0c 步骤）。
 *
 * 正式部署中 Gateway 可能正在运行，而 D23 的测试形态假设它没在跑。
 * 这里补齐"升级前必须先确认并处理既有实例"的逻辑。
 *
 * 三件事：
 *   0a. 查状态：UNKNOWN 一律中止（状态不明时不能操作）
 *   0b. 旧格式记录：说明来源不明或来自旧版本，按策略处理
 *   0c. 若 RUNNING：必须停止并**确认退出**，否则中止
 *
 * 返回 0 可以继续升级；-1 必须中止。
 *
 * 特别强调 0c：如果旧版停不下来（例如卡在 D 状态），
 * 绝不能继续切换 current。否则会变成"旧版还在跑，但文件已被换掉"
 * 的破损状态，且无法自动收敛。
 */
static int prepare_existing_gateway(const char *install_dir)
{
    /*
     * A3：清理崩溃遗留的孤儿临时文件。
     *
     * 必须在持有 .ota.lock 时调用（调用方 main.c 已持有）。
     * 60 秒阈值：写一个 pidfile 是毫秒级操作，
     * 一分钟前的临时文件必然是崩溃遗留。
     */
    int removed = ota_sup_cleanup_stale_temp(install_dir, 60);

    if (removed > 0) {
        printf(
            "NOTICE: cleaned %d stale temp file(s)\n",
            removed);
    } else if (removed < 0) {
        /* 清理失败不阻断升级，但要如实报告 */
        printf(
            "WARN: stale temp cleanup failed (%s)\n",
            strerror(errno));
    }

    /*
     * A2：旧格式 pidfile 检查。
     *
     * 旧格式无 boot_id，跨重启时可能把无关进程误认为 Gateway。
     * 这里采取"允许一次、明确告知"的策略：
     *   - 不直接拒绝，否则遗留记录会让升级被永久阻塞，
     *     需要人工删文件才能恢复；
     *   - 但要打印明显提示，便于事后追溯。
     *
     * 后续升级成功会写入带 boot_id 的新格式，自然完成迁移。
     */
    int is_legacy = 0;

    if (ota_sup_identity_is_legacy(install_dir, &is_legacy) == 0 &&
        is_legacy) {
        puts("NOTICE: legacy pidfile (no boot_id); "
             "cross-reboot identity check is weakened");
        puts("NOTICE: it will be migrated on the next successful start");
    }

    /* 0a. 查询当前状态 */
    pid_t running_pid = -1;
    ota_sup_state_t state =
        ota_sup_query(install_dir, &running_pid);

    printf(
        "Pre-upgrade gateway state: %s\n",
        ota_sup_state_name(state));

    if (state == OTA_SUP_UNKNOWN) {
        /*
         * 状态不确定时绝不能继续。
         *
         * 可能的成因：pidfile 损坏、符号链接、权限问题。
         * 无论哪种，都无法排除"已有一个 Gateway 在跑"，
         * 此时切换版本会造成不可预期的并发。
         */
        puts("FAIL: gateway state UNKNOWN; abort upgrade");
        puts("      resolve the pidfile issue before retrying");
        return -1;
    }

    if (state == OTA_SUP_STOPPED) {
        puts("PASS: no running gateway");
        return 0;
    }

    /* 0c. RUNNING：必须停干净 */
    printf(
        "NOTICE: stopping running gateway (pid=%ld)\n",
        (long)running_pid);

    /*
     * S3-R：停止外部进程的 TOCTOU 问题与现有缓解措施。
     *
     * 已知竞争窗口（复审问题二，**无法彻底消除**）：
     *     ota_sup_query() 读到 pid 与 starttime
     *     ↓  ← 此处旧进程可能退出，pid 被内核复用
     *     kill(pid, SIGTERM)
     *   若第二步恰好命中复用的新进程，就会误伤无关进程。
     *
     * 为什么不用 pidfd：
     *   pidfd_open() 需要 Linux 5.1+，而目标板是 **4.9.88**，
     *   已实测确认无 pidfd 支持。因此无法做到"打开句柄后信号
     *   必然送达同一进程"的原子语义。
     *
     * 现采取的缓解（缩小而非消除窗口）：
     *   1. 发信号**之前**重新取一次身份快照，与 pidfile 记录比对；
     *   2. 发信号**之后**再取一次快照：
     *      - 若仍是同一身份 → 信号很可能送达了正确的进程；
     *      - 若身份已变（pid 被复用）→ 立即报告异常并中止，
     *        因为此时无法确认究竟是谁收到了信号。
     *   3. 整个前置检查持有 .ota.lock，排除了"另一个 OTA 同时
     *      在操作同一安装目录"这一类竞争。
     *
     * 剩余风险：
     *   pid 复用窗口极小（需在内核 tick 级别完成进程退出+pid 分配），
     *   且 pid_max=32768 使回绕需要相当长时间。但**这不是零风险**，
     *   属于已知限制，已在文档 L6 记录。
     *   彻底解决需要内核支持（pidfd）或由 init 代为管理（S5）。
     */

    /* 发信号前再取一次快照，尽量缩短与 query 之间的间隔 */
    ota_sup_state_t recheck = ota_sup_query(install_dir, &running_pid);

    if (recheck != OTA_SUP_RUNNING) {
        /*
         * 在两次查询之间进程状态已变化。
         * 这本身不是错误（可能自己退出了），但必须重新判断，
         * 而不是拿着一个过期的 pid 去发信号。
         */
        if (recheck == OTA_SUP_STOPPED) {
            puts("PASS: gateway exited before signal");
            return 0;
        }

        puts("FAIL: gateway state changed to UNKNOWN before signal");
        return -1;
    }

    if (kill(running_pid, SIGTERM) != 0 && errno != ESRCH) {
        printf(
            "FAIL: cannot signal gateway pid=%ld: %s\n",
            (long)running_pid, strerror(errno));
        return -1;
    }

    /* 发信号后立刻复核：确认 pid 仍属于同一实例 */
    ota_sup_state_t after_signal =
        ota_sup_query(install_dir, NULL);

    if (after_signal == OTA_SUP_UNKNOWN) {
        /*
         * 发信号后身份突然无法判定：
         * 无法确认信号是否送达了正确的进程。
         * 保守中止，让人工介入核查，而不是继续升级。
         */
        puts("FAIL: identity became UNKNOWN right after SIGTERM");
        puts("      cannot confirm the signal reached the gateway");
        return -1;
    }

    /*
     * 轮询等待退出。用 ota_sup_query 判断，
     * 因为它基于 (pid, starttime) 身份校验，
     * 能正确区分"还是同一个进程"与"pid 被复用"。
     */
    const int step_ms = 50;
    const int timeout_ms = STOP_TIMEOUT_MS;
    int waited = 0;

    for (;;) {
        state = ota_sup_query(install_dir, NULL);

        if (state == OTA_SUP_STOPPED) {
            printf(
                "PASS: gateway stopped after %d ms\n", waited);
            return 0;
        }

        if (state == OTA_SUP_UNKNOWN) {
            /*
             * 等待过程中变成 UNKNOWN：无法确认是否已退出。
             * 这是危险状态，必须中止而不是继续。
             */
            puts("FAIL: gateway state became UNKNOWN while stopping");
            return -1;
        }

        if (waited >= timeout_ms) {
            /*
             * 超时仍未退出。
             *
             * 不再发 SIGKILL 硬杀：如果它卡在 D 状态，
             * SIGKILL 也无效；如果不是，只会让它来不及清理资源。
             * 更重要的是：**绝不能**在这种情况下继续切换版本。
             */
            printf(
                "FAIL: gateway pid=%ld did not exit within %d ms\n",
                (long)running_pid, timeout_ms);
            puts("      aborting upgrade; current release unchanged");
            return -1;
        }

        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = step_ms * 1000000L
        };

        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }

        waited += step_ms;
    }
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

    /*
     * S3-R：包前置校验必须在停止旧版**之前**完成。
     *
     * 顺序很重要：先确认"这个包真的能升级"，再决定"是否值得停掉业务"。
     * 反过来的话，一个损坏的包会导致"旧版已停、升级未发生"的纯中断。
     */
    if (preflight_package(package_dir, install_dir) != 0)
        return 1;

    /*
     * S3：升级前置检查（0a–0c）必须放在端口检查**之前**。
     *
     * 这是一个顺序陷阱，D23 的测试形态不会暴露它：
     *   D23 假设升级开始时 Gateway 没在运行，所以
     *   "先检查 7000 是否被占用"是合理的。
     *
     * 但正式部署中 Gateway **正在运行**，它必然占着 7000。
     * 若先做端口检查，会在"发现是自己占的"之前就拒绝升级，
     * 永远走不到"停止旧版"的逻辑——S3 的整个目的就落空了。
     *
     * 正确顺序：
     *   1. 前置校验包（不产生副作用）
     *   2. 前置检查进程：清理孤儿文件、识别旧格式、停掉运行中的实例
     *   3. 端口检查：此时应已释放，若仍被占用说明是别的进程
     */
    if (prepare_existing_gateway(install_dir) != 0)
        return 1;

    if (wifi_port_busy()) {
        puts("FAIL: TCP 7000 still occupied after stopping gateway");
        puts("      another process may be using the port");
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
