#define _GNU_SOURCE

#include "service_supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "service_ipc.h"

#define PIDFILE_MAX 128
#define CONFIG_LINE_MAX 512

/* ------------------------------------------------------------------ */
/* 小工具                                                              */
/* ------------------------------------------------------------------ */

static long long now_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    return (long long)ts.tv_sec * 1000 +
           ts.tv_nsec / 1000000;
}

/*
 * 一次读 /proc/<pid>/stat，同时取 state 与 starttime。
 *
 * 必须一次读完：分两次读会产生"快照错配"——两次读取之间
 * 进程可能退出并被复用，于是 state 来自新进程而 starttime
 * 来自旧进程，结论毫无意义。
 *
 * comm 可能含空格与 ')'，必须从**最后一个** ')' 之后开始切分。
 * 用 strchr 找第一个 ')' 或按空白分列都会静默取到错误值。
 */
static int read_stat(pid_t pid, char *state, unsigned long long *starttime)
{
    if (pid <= 0)
        return -1;

    char path[64];
    int n = snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);

    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    char buf[1024];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);

    (void)close(fd);

    if (r <= 0)
        return -1;

    buf[r] = '\0';

    const char *rp = strrchr(buf, ')');

    if (rp == NULL || rp[1] != ' ')
        return -1;

    /*
     * ')' 之后的 token 序列：state(第3) ppid(4) ... starttime(22)
     * 因此 starttime 是 ')' 之后的第 20 个 token。
     */
    char tail[768];
    snprintf(tail, sizeof(tail), "%s", rp + 2);

    char *saveptr = NULL;
    char *token = strtok_r(tail, " ", &saveptr);

    if (token == NULL)
        return -1;

    if (state != NULL)
        *state = token[0];

    for (int i = 1; i <= 19; i++) {
        token = strtok_r(NULL, " ", &saveptr);

        if (token == NULL)
            return -1;

        if (i == 19) {
            if (starttime != NULL) {
                char *end = NULL;

                errno = 0;
                unsigned long long v = strtoull(token, &end, 10);

                /* ERANGE 必须拒绝：溢出时 strtoull 返回 ULLONG_MAX */
                if (errno != 0 || end == token)
                    return -1;

                *starttime = v;
            }
        }
    }

    return 0;
}

/*
 * 僵尸与正在退出的进程都算"不是活着的服务"。
 *
 * 只判断 /proc 是否存在是不够的：僵尸进程的 /proc 目录会一直
 * 留在那里，直到被父进程回收。若把僵尸当成 RUNNING，服务
 * 实际上已经死了，而 supervisor 会拒绝重启它。
 */
static int state_is_alive(char c)
{
    return c != 'Z' && c != 'X' && c != 'x';
}

static int pidfile_path(
    const char *run_dir, char *out, size_t cap)
{
    int n = snprintf(
        out, cap, "%s/%s", run_dir, SERVICE_PIDFILE_NAME);

    return n >= 0 && (size_t)n < cap ? 0 : -1;
}

static int socket_dir_ensure(const char *run_dir)
{
    if (mkdir(run_dir, 0755) != 0 && errno != EEXIST)
        return -1;

    struct stat st;

    if (stat(run_dir, &st) != 0 || !S_ISDIR(st.st_mode)) {
        errno = ENOTDIR;
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 配置                                                                */
/* ------------------------------------------------------------------ */

static void trim_inplace(char *s)
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

int service_config_load(
    const char *config_path,
    service_config_t *config_out)
{
    if (config_path == NULL || config_out == NULL)
        return -1;

    /* 默认值：除 executable 外都有合理缺省 */
    memset(config_out, 0, sizeof(*config_out));
    config_out->baud = 115200;
    snprintf(config_out->server, sizeof(config_out->server),
             "%s", "127.0.0.1");
    config_out->port = 9000;
    config_out->stop_timeout_ms = 5000;

    FILE *fp = fopen(config_path, "re");

    if (fp == NULL)
        return -1;

    char line[CONFIG_LINE_MAX];
    int saw_executable = 0;

    while (fgets(line, sizeof(line), fp) != NULL) {
        /* 去注释 */
        char *hash = strchr(line, '#');

        if (hash != NULL)
            *hash = '\0';

        trim_inplace(line);

        if (line[0] == '\0')
            continue;

        char *eq = strchr(line, '=');

        if (eq == NULL)
            continue;

        *eq = '\0';

        char *key = line;
        char *value = eq + 1;

        trim_inplace(key);
        trim_inplace(value);

        if (strcmp(key, "executable") == 0) {
            if (value[0] != '/') {
                /* 相对路径依赖 cwd，daemon 化后 cwd 会变，必须拒绝 */
                fprintf(stderr,
                        "FAIL: executable must be an absolute path\n");
                (void)fclose(fp);
                return -1;
            }

            snprintf(config_out->executable,
                     sizeof(config_out->executable), "%s", value);
            saw_executable = 1;
        } else if (strcmp(key, "serial") == 0) {
            snprintf(config_out->serial,
                     sizeof(config_out->serial), "%s", value);
        } else if (strcmp(key, "baud") == 0) {
            char *end = NULL;
            long v = strtol(value, &end, 10);

            if (end == value || *end != '\0' || v <= 0 || v > 4000000) {
                fprintf(stderr, "FAIL: invalid baud\n");
                (void)fclose(fp);
                return -1;
            }

            config_out->baud = (int)v;
        } else if (strcmp(key, "server") == 0) {
            snprintf(config_out->server,
                     sizeof(config_out->server), "%s", value);
        } else if (strcmp(key, "port") == 0) {
            char *end = NULL;
            long v = strtol(value, &end, 10);

            if (end == value || *end != '\0' || v <= 0 || v > 65535) {
                fprintf(stderr, "FAIL: invalid port\n");
                (void)fclose(fp);
                return -1;
            }

            config_out->port = (int)v;
        } else if (strcmp(key, "stop_timeout_ms") == 0) {
            char *end = NULL;
            long v = strtol(value, &end, 10);

            if (end == value || *end != '\0' || v <= 0) {
                fprintf(stderr, "FAIL: invalid stop_timeout_ms\n");
                (void)fclose(fp);
                return -1;
            }

            config_out->stop_timeout_ms = (int)v;
        }
        /* 未知键忽略：便于向前兼容，不做严格拒绝 */
    }

    (void)fclose(fp);

    if (!saw_executable) {
        fprintf(stderr,
                "FAIL: config has no executable entry: %s\n",
                config_path);
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int service_config_write_default(
    const char *config_path,
    const char *executable)
{
    if (config_path == NULL || executable == NULL)
        return -1;

    /* O_EXCL：绝不覆盖已有配置，避免抹掉现场调整 */
    int fd = open(
        config_path,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0644);

    if (fd < 0)
        return -1;

    char payload[CONFIG_LINE_MAX * 2];

    int n = snprintf(
        payload, sizeof(payload),
        "# SmartGateway service configuration\n"
        "# Managed by service_supervisor. Edit with care.\n"
        "\n"
        "executable=%s\n"
        "\n"
        "# Serial device. Leave empty to run without serial (--no-serial).\n"
        "serial=%s\n"
        "baud=115200\n"
        "\n"
        "server=127.0.0.1\n"
        "port=9000\n"
        "\n"
        "# SIGTERM grace period before SIGKILL.\n"
        "stop_timeout_ms=5000\n",
        executable,
        "/dev/ttymxc2");

    if (n < 0 || (size_t)n >= sizeof(payload)) {
        (void)close(fd);
        errno = ENOSPC;
        return -1;
    }

    ssize_t w = write(fd, payload, (size_t)n);

    if (w != (ssize_t)n) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(config_path);
        errno = saved ? saved : EIO;
        return -1;
    }

    (void)fsync(fd);
    (void)close(fd);

    return 0;
}

/* ------------------------------------------------------------------ */
/* 状态                                                                */
/* ------------------------------------------------------------------ */

int service_state_query(
    const char *run_dir,
    service_state_t *state_out)
{
    if (run_dir == NULL || state_out == NULL)
        return -1;

    memset(state_out, 0, sizeof(*state_out));

    char path[PATH_MAX];

    if (pidfile_path(run_dir, path, sizeof(path)) != 0)
        return -1;

    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0) {
        if (errno == ENOENT)
            return 0;      /* 从未启动：等价于 STOPPED */

        return -1;
    }

    char buf[PIDFILE_MAX + 1];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);

    (void)close(fd);

    if (r <= 0)
        return -1;

    buf[r] = '\0';

    char *end = NULL;

    errno = 0;
    long long pid_value = strtoll(buf, &end, 10);

    if (errno != 0 || end == buf || pid_value <= 0 ||
        pid_value > INT_MAX)
        return -1;

    if (*end != ' ')
        return -1;

    char *st_end = NULL;

    errno = 0;
    unsigned long long recorded =
        strtoull(end + 1, &st_end, 10);

    if (errno != 0 || st_end == end + 1 || recorded == 0)
        return -1;

    /*
     * 与实时 /proc 比对。
     *
     * 必须同时校验 state：僵尸进程的 /proc 目录仍在，
     * 且 starttime 不会变，只看 starttime 会把僵尸判为存活。
     */
    char live_state = 0;
    unsigned long long live_start = 0;

    if (read_stat((pid_t)pid_value, &live_state, &live_start) != 0)
        return 0;      /* /proc 不存在：已退出 */

    if (!state_is_alive(live_state))
        return 0;      /* 僵尸/正在退出：不算在运行 */

    if (live_start != recorded)
        return 0;      /* pid 已复用：不是当初那个进程 */

    state_out->running = 1;
    state_out->pid = (pid_t)pid_value;
    state_out->starttime = live_start;

    return 0;
}

/* ------------------------------------------------------------------ */
/* Supervisor 运行时                                                   */
/* ------------------------------------------------------------------ */

typedef struct {
    const char *run_dir;
    const char *config_path;
    service_config_t config;

    pid_t child;                    /* Gateway pid，0 表示无 */
    unsigned long long child_start; /* 启动时记录的 starttime */

    /*
     * adopted：该 Gateway 是"领养"来的，不是本 supervisor fork 的。
     *
     * 触发场景：supervisor 被 kill -9 后由 init respawn 重新拉起，
     * 此时上一代 supervisor 启动的 Gateway 成了孤儿仍在运行。
     *
     * 为真的后果（必须区别对待）：
     *   - waitpid 对它无效（不是子进程），不能用 waitpid 判断退出
     *   - 只能靠 kill + 轮询 /proc 确认停止，弱于原生管理
     *   - 它退出后不会产生 SIGCHLD，也收不到僵尸
     *
     * 之所以还要支持，是因为在 respawn 模式下**没有别的选择**：
     * 若拒绝启动，init 会无限重拉一个必然失败的命令。
     */
    int adopted;
} supervisor_t;

/*
 * 信号标志。
 *
 * 用 volatile sig_atomic_t：这是 C 标准保证异步信号安全的唯一
 * 类型。普通 int 在多线程/信号场景下可能被优化掉或被撕裂。
 */
static volatile sig_atomic_t g_stop_requested = 0;
static volatile sig_atomic_t g_reload_requested = 0;

static void on_signal(int sig)
{
    (void)sig;

    /*
     * 只置标志，不做任何实际工作。
     *
     * 在信号处理函数里调用 printf/malloc/close 都是未定义行为
     * （可能死锁：主循环可能正持有 stdio 或 malloc 的锁）。
     * 主循环用 ppoll 带超时，靠超时轮询这个标志。
     */
    g_stop_requested = 1;
}

static void on_sighup(int sig)
{
    (void)sig;
    g_reload_requested = 1;
}

static int write_pidfile(
    supervisor_t *self, pid_t pid, unsigned long long starttime)
{
    char path[PATH_MAX];

    if (pidfile_path(self->run_dir, path, sizeof(path)) != 0)
        return -1;

    /*
     * 临时名带 pid：多个 supervisor 同时启动时不会互相截断。
     * （OTA 的 pidfile 曾在这里踩过一次，见 D25-2-S2R2。）
     */
    char temp[PATH_MAX];

    int n = snprintf(
        temp, sizeof(temp),
        "%s.tmp.%ld", path, (long)getpid());

    if (n < 0 || (size_t)n >= sizeof(temp))
        return -1;

    int fd = open(
        temp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (fd < 0)
        return -1;

    char payload[64];

    int p = snprintf(
        payload, sizeof(payload),
        "%ld %llu\n", (long)pid, starttime);

    if (p < 0 || (size_t)p >= sizeof(payload)) {
        (void)close(fd);
        (void)unlink(temp);
        errno = ENOSPC;
        return -1;
    }

    ssize_t w = write(fd, payload, (size_t)p);

    if (w != (ssize_t)p || fsync(fd) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(temp);
        errno = saved ? saved : EIO;
        return -1;
    }

    (void)close(fd);

    if (rename(temp, path) != 0) {
        int saved = errno;
        (void)unlink(temp);
        errno = saved;
        return -1;
    }

    /* 目录项落盘 */
    int dirfd = open(self->run_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (dirfd >= 0) {
        (void)fsync(dirfd);
        (void)close(dirfd);
    }

    return 0;
}

static void remove_pidfile(supervisor_t *self)
{
    char path[PATH_MAX];

    if (pidfile_path(self->run_dir, path, sizeof(path)) != 0)
        return;

    (void)unlink(path);
}

/*
 * 启动 Gateway。
 *
 * 返回 0 成功；-1 失败（errno 说明原因）。
 * 已在运行返回 -1 且 errno == EALREADY。
 */
static int start_child(
    supervisor_t *self,
    const service_ipc_call_t *call,
    service_ipc_reply_t *reply)
{
    if (self->child > 0) {
        /* 先确认这个 pid 是否还真实存在（可能已被外部杀掉） */
        char st = 0;
        unsigned long long live = 0;

        if (read_stat(self->child, &st, &live) == 0 &&
            state_is_alive(st) && live == self->child_start) {
            errno = EALREADY;
            return -1;
        }

        /* 残留记录：回收掉再启动 */
        int status = 0;
        (void)waitpid(self->child, &status, WNOHANG);
        self->child = 0;
        self->adopted = 0;
    }

    char *argv[10];
    int argc = 0;

    char baud_buf[16];
    char port_buf[16];

    argv[argc++] = self->config.executable;

    if (self->config.serial[0] != '\0') {
        argv[argc++] = (char *)"--serial";
        argv[argc++] = self->config.serial;
        argv[argc++] = (char *)"--baud";

        snprintf(baud_buf, sizeof(baud_buf), "%d", self->config.baud);
        argv[argc++] = baud_buf;
    } else {
        argv[argc++] = (char *)"--no-serial";
    }

    /*
     * 允许本次启动覆盖北向地址。
     *
     * 用途：OTA 的健康检查需要让 Gateway 连到一个测试用上游。
     * 覆盖只对本次启动有效，**不写回配置文件** —— 否则一次健康
     * 检查会把生产配置永久改成测试地址。
     */
    const char *server = self->config.server;
    int port = self->config.port;

    if (call != NULL && call->upstream_port > 0 &&
        call->upstream_server[0] != '\0') {
        server = call->upstream_server;
        port = call->upstream_port;
    }

    argv[argc++] = (char *)"--server";
    argv[argc++] = (char *)server;
    argv[argc++] = (char *)"--port";

    snprintf(port_buf, sizeof(port_buf), "%d", port);
    argv[argc++] = port_buf;

    argv[argc] = NULL;

    pid_t pid = fork();

    if (pid < 0)
        return -1;

    if (pid == 0) {
        /*
         * 子进程：脱离 supervisor 的会话，但仍由它作为父进程。
         *
         * 不 setsid：那会让 Gateway 成为新会话首进程，与
         * supervisor 的父子关系还在（setsid 不影响父子关系），
         * 但会让它脱离控制终端的信号——这本身没问题，
         * 却会让"supervisor 退出后 Gateway 收到 SIGHUP"的行为
         * 变得依赖终端是否存在，不可预期。
         */
        execv(self->config.executable, argv);

        /* execv 失败：直接退出，父进程通过 waitpid 感知 */
        _exit(127);
    }

    /*
     * 采集 starttime。
     *
     * 有极小概率子进程启动后立即退出，此时读不到 stat。
     * 必须当作失败处理，而不是写一个 starttime=0 的无效记录
     * ——那会让后续 query 永远无法匹配。
     */
    unsigned long long st = 0;

    if (read_stat(pid, NULL, &st) != 0) {
        int saved = errno;

        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);

        errno = saved;
        return -1;
    }

    self->child = pid;
    self->child_start = st;

    if (write_pidfile(self, pid, st) != 0) {
        int saved = errno;

        (void)kill(pid, SIGTERM);

        int status = 0;
        long long deadline = now_ms() + 2000;

        while (now_ms() < deadline) {
            if (waitpid(pid, &status, WNOHANG) == pid)
                break;

            usleep(20000);
        }

        if (self->child != 0)
            (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, WNOHANG);

        self->child = 0;
        errno = saved;
        return -1;
    }

    if (reply != NULL) {
        reply->result = SERVICE_IPC_OK;
        reply->pid = pid;
        reply->starttime = st;

        snprintf(reply->detail, sizeof(reply->detail),
                 "started");
    }

    return 0;
}

/*
 * 停止 Gateway。
 *
 * 这是 supervisor 存在的核心理由：它停的是**自己的子进程**，
 * 因此不存在"pid 被复用导致误杀"的可能。
 *
 * 关于这个保证的依据：一个已经退出但尚未被父进程 wait 的子进程
 * 处于僵尸状态，其 pid **不会**被内核重新分配。因此在
 * "child > 0 且尚未回收"的窗口内，self->child 一定还指向
 * 那个进程（或它的僵尸）。
 */
/*
 * child_gone - 判断当前 Gateway 是否已经退出
 *
 * 两种归属关系必须用不同手段判断，这是本函数存在的唯一理由：
 *
 *   原生子进程：waitpid 是权威来源。它能同时完成"判断退出"和
 *               "回收僵尸"两件事，不存在观察与被观察之间的时间差。
 *
 *   领养进程  ：waitpid 会返回 ECHILD（不是我的子进程）。
 *               只能轮询 /proc/<pid>，并用 starttime 复核身份 ——
 *               否则一个已被复用的 pid 会让它永远认为"还活着"。
 *
 * 返回 1 已退出（或身份已变）、0 仍存活、-1 无法判定。
 */
static int child_gone(supervisor_t *self)
{
    if (self->child <= 0)
        return 1;

    if (!self->adopted) {
        int status = 0;
        pid_t r = waitpid(self->child, &status, WNOHANG);

        if (r == self->child)
            return 1;

        if (r < 0 && errno == EINTR)
            return 0;

        return 0;
    }

    /*
     * 领养进程：轮询 /proc 并复核 starttime。
     *
     * 只判断 /proc 是否存在是不够的：僵尸目录会残留，
     * 而 pid 复用时目录也存在但属于另一个进程。
     */
    char st = 0;
    unsigned long long live = 0;

    if (read_stat(self->child, &st, &live) != 0)
        return 1;                    /* /proc 不存在：已退出 */

    if (!state_is_alive(st))
        return 1;                    /* 僵尸/正在退出 */

    if (live != self->child_start)
        return 1;                    /* pid 已复用：不是当初那个 */

    return 0;
}

static int stop_child(supervisor_t *self, service_ipc_reply_t *reply)
{
    if (self->child <= 0) {
        errno = ESRCH;
        return -1;
    }

    /* 可能已经自己退出 */
    if (child_gone(self) == 1) {
        self->child = 0;
        self->adopted = 0;
        remove_pidfile(self);

        if (reply != NULL) {
            reply->result = SERVICE_IPC_OK;
            snprintf(reply->detail, sizeof(reply->detail),
                     "already exited");
        }

        return 0;
    }

    /*
     * ============================================================
     * 发信号前的身份复核（S5-R2 问题三）
     * ============================================================
     *
     * 上面的 child_gone() 与下面的 kill() 之间有一个窗口：
     * 进程可能恰好在这几微秒内退出，pid 被内核重新分配。
     * 若不复核，信号会打到一个完全无关的进程上。
     *
     * 这个风险在两种归属关系下严重程度不同：
     *
     *   原生子进程：
     *     风险**结构上不存在**。已退出但未被 wait 的子进程
     *     处于僵尸态，pid 不会被复用。因此在"未回收"期间，
     *     self->child 必然仍指向那个进程或它的僵尸。
     *     kill 它最多是"给僵尸发信号"（无害，返回 ESRCH）。
     *
     *   领养进程（adopted=1）：
     *     风险**真实存在**。它退出后由 init 或它的实际父进程
     *     回收，pid 可以立刻被复用。我们没有"未回收"这层保护。
     *
     * 因此领养路径必须每次发信号前重新读 /proc 复核身份。
     * 这不是"更仔细"，而是补上一条本来就缺失的不变量。
     *
     * 局限（必须明确，记为 L13）：
     *   复核只能把窗口缩小到"read_stat 返回 与 kill 之间"，
     *   无法彻底消除 —— 那需要 pidfd（Linux 5.1+，板端 4.9.88
     *   不可用）。因此领养路径的停止操作是**弱保证**：
     *   极小概率下仍可能误伤无关进程。
     *   缓解办法是尽量让 supervisor 不进入领养状态：正常情况下
     *   respawn 拉起的 supervisor 会发现 Gateway 已随上一代
     *   supervisor 一起退出（因为它是其子进程，收到同一组信号），
     *   从而无需领养。
     */
    if (self->adopted) {
        char st = 0;
        unsigned long long live = 0;

        if (read_stat(self->child, &st, &live) != 0 ||
            !state_is_alive(st) ||
            live != self->child_start) {
            /*
             * 目标已消失或身份已变：不能发信号。
             * 按"已退出"处理 —— 我们本来就是要让它停，
             * 它已经不在了，目标达成。
             */
            self->child = 0;
            self->adopted = 0;
            remove_pidfile(self);

            if (reply != NULL) {
                reply->result = SERVICE_IPC_OK;
                snprintf(reply->detail, sizeof(reply->detail),
                         "already exited (identity changed)");
            }

            return 0;
        }
    }

    if (kill(self->child, SIGTERM) != 0 && errno != ESRCH) {
        int saved = errno;
        errno = saved;
        return -1;
    }

    long long deadline = now_ms() + self->config.stop_timeout_ms;
    int forced = 0;

    for (;;) {
        if (child_gone(self) == 1)
            break;

        if (now_ms() >= deadline) {
            if (!forced) {
                /*
                 * SIGTERM 未生效：升级为 SIGKILL。
                 *
                 * 领养路径同样需要复核 —— 从上次复核到这里
                 * 已经过了 stop_timeout_ms（默认 5 秒），
                 * 窗口比 SIGTERM 那次大得多。
                 */
                if (self->adopted) {
                    char st = 0;
                    unsigned long long live = 0;

                    if (read_stat(self->child, &st, &live) != 0 ||
                        !state_is_alive(st) ||
                        live != self->child_start) {
                        /* 目标已消失：停止目标已达成 */
                        break;
                    }
                }

                if (kill(self->child, SIGKILL) != 0 &&
                    errno != ESRCH)
                    return -1;

                forced = 1;
                deadline = now_ms() + 2000;
                continue;
            }

            /*
             * SIGKILL 都无效：只能是不间断睡眠（D）。
             * 此时不能宣称停止成功，也不能回收 pid
             * （pid 仍被占用，回收会失败）。
             */
            errno = ETIMEDOUT;
            return -1;
        }

        usleep(20000);
    }

    self->child = 0;
    self->adopted = 0;
    remove_pidfile(self);

    if (reply != NULL) {
        reply->result = SERVICE_IPC_OK;
        reply->pid = 0;
        reply->starttime = 0;

        snprintf(reply->detail, sizeof(reply->detail),
                 forced ? "stopped (SIGKILL)" : "stopped");
    }

    return 0;
}

/*
 * 处理一条请求。
 */
static void handle_request(
    supervisor_t *self,
    const service_ipc_call_t *call,
    service_ipc_reply_t *reply)
{
    memset(reply, 0, sizeof(*reply));

    switch (call->request) {
    case SERVICE_IPC_STATUS:
        reply->result = SERVICE_IPC_OK;

        if (self->child > 0) {
            reply->pid = self->child;
            reply->starttime = self->child_start;
            snprintf(reply->detail, sizeof(reply->detail), "running");
        } else {
            snprintf(reply->detail, sizeof(reply->detail), "stopped");
        }
        break;

    case SERVICE_IPC_START:
        if (start_child(self, call, reply) != 0) {
            if (errno == EALREADY) {
                /*
                 * 不要复用 strerror(EALREADY)。
                 *
                 * EALREADY 的字符串是 "Operation already in progress"，
                 * 那是 socket 场景的措辞，用在"服务已在运行"上会让
                 * 运维以为有另一个操作正在执行（板端实测已确认这点）。
                 * 幂等启动是正常结果，不是错误。
                 */
                reply->result = SERVICE_IPC_ALREADY;
                reply->pid = self->child;
                reply->starttime = self->child_start;

                snprintf(reply->detail, sizeof(reply->detail),
                         "already running (pid=%ld)",
                         (long)self->child);
            } else {
                reply->result = SERVICE_IPC_FAILED;
                snprintf(reply->detail, sizeof(reply->detail),
                         "%s", strerror(errno));
            }
        }
        break;

    case SERVICE_IPC_STOP:
        if (stop_child(self, reply) != 0) {
            if (errno == ESRCH) {
                reply->result = SERVICE_IPC_NOT_RUNNING;
                snprintf(reply->detail, sizeof(reply->detail),
                         "not running");
            } else {
                reply->result = SERVICE_IPC_FAILED;
                snprintf(reply->detail, sizeof(reply->detail),
                         "%s", strerror(errno));
            }
        }
        break;

    case SERVICE_IPC_RELOAD:
        /*
         * 重载真的重新读配置，而不是只回一个 OK。
         *
         * 只回 OK 会让调用方以为新配置已生效——这是假阳性。
         * 重载只更新"下次启动才生效"的参数；正在运行的进程
         * 参数无法改变，且不应该重启它（那是 OTA 的职责）。
         */
        {
            service_config_t fresh;

            if (service_config_load(
                    self->config_path, &fresh) != 0) {
                reply->result = SERVICE_IPC_FAILED;
                snprintf(reply->detail, sizeof(reply->detail),
                         "cannot reload config: %s",
                         strerror(errno));
                break;
            }

            self->config = fresh;

            reply->result = SERVICE_IPC_OK;
            snprintf(reply->detail, sizeof(reply->detail),
                     "config reloaded (applies on next start)");
        }
        break;

    default:
        reply->result = SERVICE_IPC_BAD_REQUEST;
        snprintf(reply->detail, sizeof(reply->detail),
                 "unknown request");
        break;
    }
}

int service_supervisor_run(
    const char *run_dir,
    const char *config_path,
    int foreground)
{
    (void)foreground;

    if (run_dir == NULL || config_path == NULL)
        return -1;

    if (socket_dir_ensure(run_dir) != 0) {
        perror("FAIL: cannot create run directory");
        return -1;
    }

    supervisor_t self;
    memset(&self, 0, sizeof(self));

    self.run_dir = run_dir;
    self.config_path = config_path;

    if (service_config_load(config_path, &self.config) != 0) {
        perror("FAIL: cannot load config");
        return -1;
    }

    /*
     * 启动前先看有没有残留的 pidfile。
     *
     * 场景：supervisor 被 kill -9，它启动的 Gateway 成了孤儿
     * 但仍在运行，pidfile 也还在。新拉起的 supervisor 面对的是
     * 一个"不是我启动的 Gateway"。
     *
     * 两种处置方式，选择取决于运行模式：
     *
     *   交互模式（手工启动）：
     *     报告并要求人工处理。此时人可以判断那个 Gateway 是否
     *     该留、数据是否重要。
     *
     *   respawn 模式（由 init 拉起）：
     *     **必须能自愈**，否则 init 会无限重拉一个必然失败的
     *     命令，形成永久重启循环 —— 那比"服务没起来"更糟，
     *     因为它会持续占日志、持续消耗 CPU。
     *
     * 领养的安全前提：
     *   pidfile 里的 (pid, starttime) 与 /proc 实时值一致，
     *   说明那确实是当初记录的那个进程（不是被复用的 pid）。
     *   这个身份校验由 service_state_query 完成。
     *
     * 领养的局限（必须明确，写进文档）：
     *   被领养的进程**不是本 supervisor 的子进程**，因此
     *   waitpid 对它无效。stop_child() 里依赖 waitpid 的部分
     *   对它不成立 —— 为此引入 adopted 标志，走信号 + 轮询
     *   /proc 的路径，并在文档里记录其弱于原生管理。
     */
    service_state_t previous;

    if (service_state_query(run_dir, &previous) != 0) {
        perror("FAIL: cannot query previous state");
        return -1;
    }

    if (previous.running) {
        const char *respawn_mode = getenv("SMARTGATEWAY_RESPAWN");
        int under_respawn = respawn_mode != NULL &&
                            respawn_mode[0] == '1';

        if (!under_respawn) {
            fprintf(stderr,
                    "FAIL: a gateway is already running (pid=%ld) but this\n"
                    "      supervisor did not start it and cannot manage it.\n"
                    "      stop it manually, or set SMARTGATEWAY_RESPAWN=1\n"
                    "      to let the supervisor adopt it.\n",
                    (long)previous.pid);
            return -1;
        }

        /*
         * respawn 模式：领养孤儿。
         *
         * 保留原 pidfile（service_state_query 已确认身份一致），
         * 只在内存中记为子进程 —— 这样 STATUS 能正确报告，
         * 停止时也能把信号发给正确的 pid。
         */
        printf("supervisor: adopting orphaned gateway (pid=%ld)\n",
               (long)previous.pid);
        fflush(stdout);

        self.child = previous.pid;
        self.child_start = previous.starttime;
        self.adopted = 1;
    }

    char socket_path[128];
    int listen_fd = service_ipc_listen(
        run_dir, socket_path, sizeof(socket_path));

    if (listen_fd < 0) {
        if (errno == EADDRINUSE) {
            fprintf(stderr,
                    "FAIL: another supervisor is already running\n");
        } else {
            perror("FAIL: cannot create socket");
        }

        return -1;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    /* 不设 SA_RESTART：让 ppoll 能被打断 */

    (void)sigaction(SIGTERM, &sa, NULL);
    (void)sigaction(SIGINT, &sa, NULL);

    struct sigaction hup;
    memset(&hup, 0, sizeof(hup));
    hup.sa_handler = on_sighup;
    sigemptyset(&hup.sa_mask);
    (void)sigaction(SIGHUP, &hup, NULL);

    /* 忽略 SIGPIPE：客户端提前关闭时不应杀死 supervisor */
    (void)signal(SIGPIPE, SIG_IGN);

    printf("supervisor: listening on %s\n", socket_path);
    printf("supervisor: executable=%s\n", self.config.executable);
    fflush(stdout);

    while (!g_stop_requested) {
        /*
         * 用 ppoll 带超时，而不是无限期阻塞在 accept。
         *
         * 原因：信号处理器只置标志，若 accept 无限阻塞，
         * 收到 SIGTERM 后进程不会退出（除非信号打断 accept，
         * 但那是平台相关的隐含行为，不该依赖）。
         */
        struct pollfd pfd = {
            .fd = listen_fd,
            .events = POLLIN,
            .revents = 0,
        };

        struct timespec timeout = { .tv_sec = 0, .tv_nsec = 200000000 };

        int ready = ppoll(&pfd, 1, &timeout, NULL);

        /*
         * 顺带回收子进程。
         *
         * 不这样做会留下僵尸，且 self->child 会指向一个
         * 已死的 pid —— 后续 STATUS 会谎报 running。
         *
         * 用 child_gone() 而不是直接 waitpid：领养来的 Gateway
         * 不是本进程的子进程，waitpid 会返回 ECHILD，那样
         * "已退出"永远检测不到，STATUS 会一直说谎。
         */
        if (self.child > 0 && child_gone(&self) == 1) {
            self.child = 0;
            self.adopted = 0;
            remove_pidfile(&self);

            printf("supervisor: gateway exited\n");
            fflush(stdout);
        }

        if (g_reload_requested) {
            g_reload_requested = 0;

            service_config_t fresh;

            if (service_config_load(config_path, &fresh) == 0) {
                /*
                 * 只更新"下次启动才生效"的参数。
                 * 正在运行的进程参数无法改变，且不应该重启它
                 * ——那是 OTA 的职责，supervisor 不擅自决定。
                 */
                self.config = fresh;
                printf("supervisor: config reloaded\n");
            } else {
                fprintf(stderr, "supervisor: config reload failed\n");
            }

            fflush(stdout);
        }

        if (ready <= 0)
            continue;

        service_ipc_call_t call;
        int client = service_ipc_accept_call(listen_fd, &call);

        if (client < 0)
            continue;

        service_ipc_reply_t reply;

        handle_request(&self, &call, &reply);

        printf("supervisor: request=%d result=%s\n",
               (int)call.request,
               service_ipc_result_name(reply.result));
        fflush(stdout);

        (void)service_ipc_send_reply_and_close(client, &reply);
    }

    /*
     * 退出前停掉子进程。
     *
     * 不这样做会留下一个"父进程已死"的 Gateway：
     * 它的 pid 会被 init 接管，之后 supervisor 重启时
     * 无法回收，也没法通过 IPC 停止它。
     */
    if (self.child > 0) {
        printf("supervisor: stopping gateway on exit\n");
        fflush(stdout);

        service_ipc_reply_t reply;
        (void)stop_child(&self, &reply);
    }

    (void)close(listen_fd);
    (void)unlink(socket_path);

    printf("supervisor: exited\n");
    fflush(stdout);

    return 0;
}
