#define _POSIX_C_SOURCE 200809L

/*
 * =====================================================================
 * tcp_client_send_all 的发送超时测试
 *
 * 背景：
 *   早年实现里 send() 是阻塞的，对端一旦长时间不读，
 *   server_link_worker 就会永久卡在 send 里：
 *     · 不再消费 upstream_queue（上报全面停滞）
 *     · 不再轮询下行 CMD
 *   网关表面还活着，实则北向已死。
 *
 * 本用例直接对该函数施压，把"发送必须有超时上限"钉死：
 *   ① 正常对端：数据必须完整送达
 *   ② 僵死对端（只连不读）：必须在约 SEND_TIMEOUT_MS 内返回失败，
 *      而不是无限阻塞
 *   ③ 参数非法：必须返回 -1
 * =====================================================================
 */

#include "tcp_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

/*
 * 与 tcp_client.c 中的 TCP_CLIENT_SEND_TIMEOUT_MS 保持一致。
 *
 * 这个数字是测试的判据来源：
 * 若生产代码去掉了 poll 超时，僵死用例的耗时断言会立刻失败。
 */
#define EXPECTED_TIMEOUT_MS 1000

static int g_failed = 0;

#define CHECK(cond, msg)                                        \
    do                                                          \
    {                                                           \
        if (!(cond))                                            \
        {                                                       \
            fprintf(stderr, "  [FAIL] %s\n", (msg));            \
            g_failed = 1;                                       \
            return;                                             \
        }                                                       \
    } while (0)

/*
 * 建立一对已连接的本地 socket。
 *
 * @param out_client 主动连接端
 * @param out_server 被动接受端
 *
 * @return 0成功，-1失败
 */
static int make_socket_pair(int *out_client, int *out_server)
{
    int listen_fd;
    int client_fd;
    int server_fd;
    int reuse = 1;

    struct sockaddr_in address;
    socklen_t address_length = sizeof(address);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (listen_fd < 0)
    {
        return -1;
    }

    setsockopt(
        listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)
    );

    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;

    if (bind(listen_fd, (struct sockaddr *)&address, sizeof(address)) != 0 ||
        listen(listen_fd, 1) != 0 ||
        getsockname(
            listen_fd, (struct sockaddr *)&address, &address_length
        ) != 0)
    {
        close(listen_fd);
        return -1;
    }

    client_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (client_fd < 0 ||
        connect(
            client_fd, (struct sockaddr *)&address, sizeof(address)
        ) != 0)
    {
        close(client_fd);
        close(listen_fd);
        return -1;
    }

    server_fd = accept(listen_fd, NULL, NULL);

    close(listen_fd);

    if (server_fd < 0)
    {
        close(client_fd);
        return -1;
    }

    *out_client = client_fd;
    *out_server = server_fd;

    return 0;
}

/*
 * 正常对端：会读取，发送必须成功且内容完整。
 */
static void test_send_succeeds_with_healthy_peer(void)
{
    int client_fd;
    int server_fd;

    char payload[] = "{\"node\":\"NODE01\",\"seq\":1}\n";
    char received[128];

    ssize_t n;

    CHECK(make_socket_pair(&client_fd, &server_fd) == 0, "socket pair");

    CHECK(
        tcp_client_send_all(client_fd, payload, strlen(payload)) == 0,
        "send to healthy peer"
    );

    n = recv(server_fd, received, sizeof(received) - 1U, 0);

    CHECK(n == (ssize_t)strlen(payload), "received length");
    received[n] = '\0';
    CHECK(strcmp(received, payload) == 0, "received content");

    close(client_fd);
    close(server_fd);

    printf("  [PASS] healthy peer: full payload delivered\n");
}

/*
 * 参数校验：非法参数必须返回 -1；length=0 视为成功。
 */
static void test_send_rejects_bad_arguments(void)
{
    CHECK(tcp_client_send_all(-1, "x", 1U) == -1, "negative fd");
    CHECK(tcp_client_send_all(-1, NULL, 0U) == -1, "negative fd, zero len");
    CHECK(tcp_client_send_all(1, NULL, 1U) == -1, "NULL data, non-zero len");

    printf("  [PASS] bad arguments rejected\n");
}

/*
 * 核心用例：对端只连不读，发送缓冲写满后必须超时返回，
 * 而不是无限阻塞。
 */
static void test_send_times_out_on_stuck_peer(void)
{
    int client_fd;
    int server_fd;

    struct timespec start;
    struct timespec finish;

    long elapsed_ms;

    char filler[8192];

    int saturated = 0;
    int i;
    int result;

    CHECK(make_socket_pair(&client_fd, &server_fd) == 0, "socket pair");

    /*
     * 把两端缓冲都压到最小，让链路尽快进入不可写状态。
     */
    {
        int small = 1024;

        setsockopt(
            server_fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)
        );
        setsockopt(
            client_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)
        );
    }

    /*
     * 先把发送缓冲灌满（非阻塞地灌，避免测试自己卡住），
     * 直到 send 返回 EAGAIN。
     */
    CHECK(
        tcp_client_set_nonblocking(client_fd) == 0,
        "set client nonblocking"
    );

    memset(filler, 'A', sizeof(filler));

    for (i = 0; i < 200000 && !saturated; i++)
    {
        ssize_t n = send(client_fd, filler, sizeof(filler), MSG_NOSIGNAL);

        if (n < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                saturated = 1;
            }
            else
            {
                break;
            }
        }
    }

    CHECK(saturated == 1, "saturate send buffer");

    /*
     * 现在调用被测函数。缓冲已满且对端不读，
     * 它必须等满超时后返回 -1。
     */
    clock_gettime(CLOCK_MONOTONIC, &start);

    errno = 0;
    result = tcp_client_send_all(client_fd, "X", 1U);

    clock_gettime(CLOCK_MONOTONIC, &finish);

    CHECK(result == -1, "send to stuck peer fails");
    CHECK(errno == ETIMEDOUT, "errno is ETIMEDOUT");

    elapsed_ms =
        (finish.tv_sec - start.tv_sec) * 1000L +
        (finish.tv_nsec - start.tv_nsec) / 1000000L;

    /*
     * 下界：必须真的等过（防止提前返回而测试形同虚设）
     * 上界：必须有上限（防止无限阻塞 —— 这正是本用例的目的）
     */
    if (elapsed_ms < EXPECTED_TIMEOUT_MS - 100L)
    {
        fprintf(
            stderr,
            "  [FAIL] returned too early: %ld ms\n",
            elapsed_ms
        );
        g_failed = 1;
        close(client_fd);
        close(server_fd);
        return;
    }

    if (elapsed_ms >= EXPECTED_TIMEOUT_MS * 4L)
    {
        fprintf(
            stderr,
            "  [FAIL] took too long: %ld ms (timeout not working?)\n",
            elapsed_ms
        );
        g_failed = 1;
        close(client_fd);
        close(server_fd);
        return;
    }

    printf(
        "  [PASS] stuck peer: timed out in %ld ms (~%d ms expected)\n",
        elapsed_ms,
        EXPECTED_TIMEOUT_MS
    );

    close(client_fd);
    close(server_fd);
}

/*
 * 阻塞模式下也必须能超时。
 *
 * 这很重要：即便调用方忘记 set_nonblocking，
 * send_all 内部的 poll 依然能兜住超时。
 */
static void test_send_times_out_without_nonblocking(void)
{
    int client_fd;
    int server_fd;

    struct timespec start;
    struct timespec finish;

    long elapsed_ms;

    char filler[8192];

    int i;
    int result;

    CHECK(make_socket_pair(&client_fd, &server_fd) == 0, "socket pair");

    {
        int small = 1024;

        setsockopt(
            server_fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)
        );
        setsockopt(
            client_fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)
        );
    }

    /*
     * 注意：这里**不**调用 set_nonblocking，
     * 保持阻塞 socket，用大块数据反复写把缓冲灌满。
     *
     * 因为对端不读，几次之后 send 会阻塞 ——
     * 所以用 SO_SNDTIMEO 做临时保护，避免测试自身卡死。
     */
    {
        struct timeval tv;

        tv.tv_sec = 0;
        tv.tv_usec = 200000;   /* 200ms，仅用于灌水阶段 */

        setsockopt(
            client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)
        );
    }

    memset(filler, 'A', sizeof(filler));

    for (i = 0; i < 200000; i++)
    {
        ssize_t n = send(client_fd, filler, sizeof(filler), MSG_NOSIGNAL);

        if (n < 0)
        {
            break;   /* 已满或超时，灌水阶段结束 */
        }
    }

    /*
     * 关键：把 SO_SNDTIMEO 清掉，
     * 确保接下来的超时完全来自 send_all 内部的 poll，
     * 而不是内核的发送超时。
     */
    {
        struct timeval tv;

        tv.tv_sec = 0;
        tv.tv_usec = 0;

        setsockopt(
            client_fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv)
        );
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    errno = 0;
    result = tcp_client_send_all(client_fd, "X", 1U);

    clock_gettime(CLOCK_MONOTONIC, &finish);

    CHECK(result == -1, "blocking socket still fails");

    elapsed_ms =
        (finish.tv_sec - start.tv_sec) * 1000L +
        (finish.tv_nsec - start.tv_nsec) / 1000000L;

    if (elapsed_ms >= EXPECTED_TIMEOUT_MS * 4L)
    {
        fprintf(
            stderr,
            "  [FAIL] blocking socket took %ld ms (poll timeout missing?)\n",
            elapsed_ms
        );
        g_failed = 1;
        close(client_fd);
        close(server_fd);
        return;
    }

    printf(
        "  [PASS] blocking socket also bounded: %ld ms\n",
        elapsed_ms
    );

    close(client_fd);
    close(server_fd);
}

int main(void)
{
    printf("Running tcp_client send-timeout tests...\n");

    test_send_rejects_bad_arguments();
    test_send_succeeds_with_healthy_peer();
    test_send_times_out_on_stuck_peer();
    test_send_times_out_without_nonblocking();

    if (g_failed)
    {
        printf("存在失败用例\n");
        return 1;
    }

    printf("All tcp_client send-timeout tests passed.\n");

    return 0;
}
