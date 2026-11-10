
#define _GNU_SOURCE

#include "ota_health.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    return (long long)ts.tv_sec * 1000LL +
           ts.tv_nsec / 1000000LL;
}

/*
 * 非破坏性检查子进程是否已退出。
 * WNOWAIT：保留退出状态，供 ota_process_stop()
 * 之后使用 waitpid() 正式回收。
 */
/*
 * child_alive - 判断进程是否仍在运行
 *
 * 历史与陷阱（S5 修正）
 * --------------------
 * 原实现用 waitid(P_PID, pid, WEXITED|WNOHANG|WNOWAIT)：
 * 它只在 **本进程是 pid 的父进程** 时有效，因为 wait* 系列
 * 的语义就是"等待自己的子进程状态变化"。
 *
 * S3/S4 期间 OTA 自己 fork Gateway，所以这个前提成立。
 * S5 之后 Gateway 由 Service Supervisor fork，OTA 不再是父进程，
 * waitid 直接返回 ECHILD —— 于是 ota_health_wait_tcp_ready()
 * 会立刻判"进程已死"。
 *
 * 实测症状：升级走完 IPC 启动、Gateway 确实在跑并监听 7000，
 * 但输出 "FAIL: L1 health" 并触发回滚。
 *
 * 这个缺陷的隐蔽之处在于：错的是**前提**，不是逻辑。
 * waitid 返回值被正确检查了，只是返回 -1 的原因变了。
 *
 * 修正：改用对"进程是否存在"的直接观察 —— 不依赖父子关系。
 *   读 /proc/<pid>/stat 的 state 字段，
 *   僵尸（Z）与正在退出（X/x）视为不存活。
 *
 * 为什么不用 kill(pid, 0)：那只能证明 pid 被占用，
 * 无法区分"活进程"与"僵尸"，而僵尸在 OTA 场景下必须判为不活
 * （否则会误认为新版在运行）。
 *
 * 需要注意：本函数只能回答"这个 pid 对应的进程还在不在"，
 * 不能回答"它还是不是当初那个进程"。后者的完整判定
 * （starttime/boot_id）在 ota_supervisor 模块里，
 * 而在 S5 架构下，这个判定已由 supervisor 自己保证。
 */
static int child_alive(pid_t pid)
{
    if (pid <= 1)
        return -1;

    char path[64];

    if (snprintf(path, sizeof(path),
                 "/proc/%ld/stat", (long)pid) >=
        (int)sizeof(path))
        return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0) {
        /* 进程不存在：正常终止路径 */
        if (errno == ENOENT)
            return 0;

        return -1;
    }

    char buffer[1024];
    ssize_t n = read(fd, buffer, sizeof(buffer) - 1);

    (void)close(fd);

    if (n <= 0)
        return -1;

    buffer[n] = '\0';

    /*
     * comm 可能含空格与 ')'，必须从**最后一个** ')' 之后取 state。
     * 用 strchr 找第一个 ')' 或按空白分列都会静默取到错误值
     * （该陷阱在 ota_supervisor 已有记录）。
     */
    const char *rp = strrchr(buffer, ')');

    if (rp == NULL || rp[1] != ' ' || rp[2] == '\0')
        return -1;

    char state = rp[2];

    /* Z 僵尸、X/x 正在退出，都不算存活 */
    if (state == 'Z' || state == 'X' || state == 'x')
        return 0;

    return 1;
}

static int tcp_probe(
    const struct sockaddr_in *address)
{
    int fd;
    int ret;
    int error = 0;
    socklen_t error_len = sizeof(error);

    fd = socket(
        AF_INET,
        SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
        0);

    if (fd < 0)
        return -1;

    ret = connect(
        fd,
        (const struct sockaddr *)address,
        sizeof(*address));

    if (ret == 0) {
        close(fd);
        return 0;
    }

    if (errno != EINPROGRESS) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    struct pollfd pfd = {
        .fd = fd,
        .events = POLLOUT
    };

    do {
        ret = poll(&pfd, 1, 100);
    } while (ret < 0 && errno == EINTR);

    if (ret <= 0) {
        int saved = ret == 0 ? ETIMEDOUT : errno;
        close(fd);
        errno = saved;
        return -1;
    }

    if (getsockopt(
            fd, SOL_SOCKET, SO_ERROR,
            &error, &error_len) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }

    close(fd);

    if (error != 0) {
        errno = error;
        return -1;
    }

    return 0;
}

int ota_health_wait_tcp_ready(
    pid_t gateway_pid,
    const char *ipv4_address,
    unsigned short port,
    int timeout_ms)
{
    struct sockaddr_in address;
    long long start;

    if (gateway_pid <= 1 ||
        ipv4_address == NULL ||
        port == 0 ||
        timeout_ms <= 0) {
        errno = EINVAL;
        return -1;
    }

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(port);

    if (inet_pton(
            AF_INET,
            ipv4_address,
            &address.sin_addr) != 1) {
        errno = EINVAL;
        return -1;
    }

    start = now_ms();

    if (start < 0)
        return -1;

    for (;;) {
        int alive = child_alive(gateway_pid);

        if (alive <= 0) {
            if (alive == 0)
                errno = ECHILD;
            return -1;
        }

        if (tcp_probe(&address) == 0) {
            /*
             * TCP 成功后再检查一次，防止 Gateway
             * 在连接成功后立即退出。
             */
            alive = child_alive(gateway_pid);

            if (alive == 1)
                return 0;

            if (alive == 0)
                errno = ECHILD;
            return -1;
        }

        long long current = now_ms();

        if (current < 0)
            return -1;

        if (current - start >= timeout_ms) {
            errno = ETIMEDOUT;
            return -1;
        }

        struct timespec delay = {
            .tv_sec = 0,
            .tv_nsec = 50 * 1000 * 1000
        };

        while (nanosleep(&delay, &delay) != 0 &&
               errno == EINTR) {
        }
    }
}
