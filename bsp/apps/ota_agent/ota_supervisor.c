#define _POSIX_C_SOURCE 200809L

#include "ota_supervisor.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "ota_process.h"

#define OTA_SUP_RUN_DIR "run"
#define OTA_SUP_PID_NAME "gateway.pid"

/*
 * 临时文件使用"固定前缀 + 进程私有后缀"。
 *
 * 不能用固定名字：两个进程同时写 pidfile 时（例如 OTA 与 init 脚本
 * 都在登记），会互相截断对方正在写的临时文件，rename 后才可能发现
 * 内容已损坏。用 pid 作后缀可保证各自的临时文件互不干扰。
 *
 * 注意：这仍需配合 .ota.lock 使用，因为 pidfile 本身的语义是
 * "唯一一份记录"，并发写只应该发生在一个持有锁的写者上。
 */
#define OTA_SUP_TEMP_PREFIX "gateway.pid.tmp."

/* pidfile 最大长度：两个十进制数 + 换行，留足余量即可 */
#define OTA_SUP_PIDFILE_MAX 128

/* ------------------------------------------------------------------ */
/* 路径与工具                                                          */
/* ------------------------------------------------------------------ */

int ota_sup_pidfile_path(
    const char *install_dir,
    char *output,
    size_t capacity)
{
    if (install_dir == NULL || install_dir[0] == '\0' ||
        output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(
        output, capacity,
        "%s/" OTA_SUP_RUN_DIR "/" OTA_SUP_PID_NAME,
        install_dir);

    if (n < 0 || (size_t)n >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

/*
 * 读取 /proc/<pid>/stat 的进程状态字符（第 3 字段）。
 *
 * 格式：pid (comm) state ppid ...
 * ')' 之后的第一个非空白字符即 state。
 */
int ota_sup_read_state(
    pid_t pid,
    char *state_out)
{
    char path[64];
    char buffer[1024];
    int fd;
    ssize_t n;

    if (pid <= 0 || state_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    int len = snprintf(
        path, sizeof(path),
        "/proc/%ld/stat", (long)pid);

    if (len < 0 || (size_t)len >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    do {
        n = read(fd, buffer, sizeof(buffer) - 1);
    } while (n < 0 && errno == EINTR);

    (void)close(fd);

    if (n < 0)
        return -1;

    if (n == 0) {
        errno = EIO;
        return -1;
    }

    buffer[n] = '\0';

    const char *close_paren = strrchr(buffer, ')');

    if (close_paren == NULL || close_paren[1] == '\0') {
        errno = EINVAL;
        return -1;
    }

    const char *cursor = close_paren + 1;

    while (*cursor == ' ')
        cursor++;

    if (*cursor == '\0') {
        errno = EINVAL;
        return -1;
    }

    *state_out = *cursor;

    return 0;
}

int ota_sup_state_is_alive(char state_char)
{
    /*
     * 'Z' 僵尸：/proc 条目尚在但进程已终止，不可视为运行。
     * 'X' / 'x'：已死亡（内核 4.9 起 x 为 X 的别名）。
     * 其余（R/S/D/T/t/W）均表示进程仍存在，保守视为运行。
     */
    if (state_char == 'Z' || state_char == 'X' || state_char == 'x')
        return 0;

    return 1;
}

/*
 * 从 /proc/<pid>/stat 解析 starttime（第 22 字段）。
 *
 * 格式：pid (comm) state ppid ... starttime ...
 *
 * 关键：comm 可能含空格与 ')'，必须从**最后一个** ')' 之后开始。
 * 用 strchr 取第一个 ')' 或按空白分列，都会静默取到错误值。
 *
 * ')' 之后按空白切分，第 1 个 token 是 state（第 3 字段），
 * 因此 starttime（第 22 字段）是第 20 个 token（索引 19）。
 */
int ota_sup_read_starttime(
    pid_t pid,
    unsigned long long *starttime_out)
{
    char path[64];
    char buffer[1024];
    int fd;
    ssize_t n;

    if (pid <= 0 || starttime_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    int len = snprintf(
        path, sizeof(path),
        "/proc/%ld/stat", (long)pid);

    if (len < 0 || (size_t)len >= sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;   /* ENOENT：进程不存在 */

    do {
        n = read(fd, buffer, sizeof(buffer) - 1);
    } while (n < 0 && errno == EINTR);

    (void)close(fd);

    if (n < 0)
        return -1;

    if (n == 0) {
        errno = EIO;
        return -1;
    }

    buffer[n] = '\0';

    const char *close_paren = strrchr(buffer, ')');

    if (close_paren == NULL || close_paren[1] == '\0') {
        errno = EINVAL;
        return -1;
    }

    const char *cursor = close_paren + 1;
    int token_index = 0;

    while (*cursor != '\0') {
        while (*cursor == ' ')
            cursor++;

        if (*cursor == '\0' || *cursor == '\n')
            break;

        if (token_index == 19) {
            char *end = NULL;

            errno = 0;

            unsigned long long value =
                strtoull(cursor, &end, 10);

            if (errno != 0 || end == cursor) {
                errno = EINVAL;
                return -1;
            }

            *starttime_out = value;
            return 0;
        }

        while (*cursor != ' ' && *cursor != '\0' &&
               *cursor != '\n')
            cursor++;

        token_index++;
    }

    /* 字段数不足：/proc 格式与预期不符 */
    errno = EINVAL;
    return -1;
}

int ota_sup_capture_identity(
    pid_t pid,
    ota_sup_identity_t *identity_out)
{
    unsigned long long starttime = 0;

    if (pid <= 0 || identity_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (ota_sup_read_starttime(pid, &starttime) != 0)
        return -1;

    identity_out->pid = pid;
    identity_out->starttime = starttime;

    return 0;
}

/* ------------------------------------------------------------------ */
/* 目录操作                                                            */
/* ------------------------------------------------------------------ */

/*
 * 打开（必要时创建）<install_dir>/run 目录。
 *
 * 返回目录 fd；失败返回 -1。
 *
 * 安全要求：
 *   - install_dir 必须是真实的目录，不跟随符号链接
 *   - run 目录同样不跟随符号链接
 *   - 创建时用 0700，避免其他用户篡改 pidfile
 */
static int open_run_dir(const char *install_dir, int create)
{
    int basefd;
    int dirfd;
    int saved_errno;

    if (install_dir == NULL || install_dir[0] == '\0') {
        errno = EINVAL;
        return -1;
    }

    basefd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

    if (basefd < 0)
        return -1;

    if (create) {
        if (mkdirat(basefd, OTA_SUP_RUN_DIR, 0700) != 0 &&
            errno != EEXIST) {
            saved_errno = errno;
            (void)close(basefd);
            errno = saved_errno;
            return -1;
        }
    }

    dirfd = openat(
        basefd,
        OTA_SUP_RUN_DIR,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);

    saved_errno = errno;
    (void)close(basefd);

    if (dirfd < 0) {
        errno = saved_errno;
        return -1;
    }

    return dirfd;
}

/* ------------------------------------------------------------------ */
/* pidfile 写入                                                        */
/* ------------------------------------------------------------------ */

int ota_sup_pidfile_write(
    const char *install_dir,
    const ota_sup_identity_t *identity)
{
    char payload[OTA_SUP_PIDFILE_MAX];
    char tmp_name[64];
    int dirfd;
    int tmpfd;
    int saved_errno;
    int len;
    size_t written = 0;
    struct stat st;

    if (identity == NULL || identity->pid <= 0 ||
        identity->starttime == 0) {
        /*
         * starttime 为 0 说明身份不完整。
         * 拒绝写入一个无法用于后续校验的记录——
         * 写进去只会让将来误判为 STOPPED。
         */
        errno = EINVAL;
        return -1;
    }

    dirfd = open_run_dir(install_dir, 1);

    if (dirfd < 0)
        return -1;

    /* 临时名带自身 pid，避免并发写者互相截断 */
    int tn = snprintf(
        tmp_name, sizeof(tmp_name),
        OTA_SUP_TEMP_PREFIX "%ld", (long)getpid());

    if (tn < 0 || (size_t)tn >= sizeof(tmp_name)) {
        (void)close(dirfd);
        errno = ENAMETOOLONG;
        return -1;
    }

    /*
     * O_EXCL：临时文件必须由本次调用独占创建。
     *
     * 若已存在同名文件（可能是上次崩溃残留，或有人预置了符号链接），
     * 直接失败而不是复用——复用一个未知来源的文件会把内容写进
     * 意料之外的位置。
     */
    tmpfd = openat(
        dirfd,
        tmp_name,
        O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
        0600);

    if (tmpfd < 0) {
        saved_errno = errno;

        /* 上次残留的临时文件：清掉后重试一次 */
        if (saved_errno == EEXIST) {
            (void)unlinkat(dirfd, tmp_name, 0);

            tmpfd = openat(
                dirfd,
                tmp_name,
                O_WRONLY | O_CREAT | O_EXCL |
                O_CLOEXEC | O_NOFOLLOW,
                0600);

            saved_errno = errno;
        }

        if (tmpfd < 0) {
            (void)close(dirfd);
            errno = saved_errno;
            return -1;
        }
    }

    /* O_CREAT 的 0600 会受 umask 影响，这里强制确认 */
    if (fstat(tmpfd, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        (st.st_mode & 0777) != 0600) {
        if (fstat(tmpfd, &st) != 0 ||
            !S_ISREG(st.st_mode) ||
            fchmod(tmpfd, 0600) != 0) {
            saved_errno = errno;
            (void)close(tmpfd);
            (void)unlinkat(dirfd, tmp_name, 0);
            (void)close(dirfd);
            errno = saved_errno;
            return -1;
        }
    }

    len = snprintf(
        payload, sizeof(payload),
        "%ld %llu\n",
        (long)identity->pid,
        identity->starttime);

    if (len < 0 || (size_t)len >= sizeof(payload)) {
        (void)close(tmpfd);
        (void)unlinkat(dirfd, tmp_name, 0);
        (void)close(dirfd);
        errno = EINVAL;
        return -1;
    }

    while (written < (size_t)len) {
        ssize_t n = write(
            tmpfd,
            payload + written,
            (size_t)len - written);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            saved_errno = (n < 0) ? errno : EIO;
            (void)close(tmpfd);
            (void)unlinkat(dirfd, tmp_name, 0);
            (void)close(dirfd);
            errno = saved_errno;
            return -1;
        }

        written += (size_t)n;
    }

    /* 先落盘数据，再改名，保证掉电后不会留下半截内容 */
    if (fsync(tmpfd) != 0) {
        saved_errno = errno;
        (void)close(tmpfd);
        (void)unlinkat(dirfd, tmp_name, 0);
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    if (close(tmpfd) != 0) {
        saved_errno = errno;
        (void)unlinkat(dirfd, tmp_name, 0);
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    if (renameat(
            dirfd, tmp_name,
            dirfd, OTA_SUP_PID_NAME) != 0) {
        saved_errno = errno;
        (void)unlinkat(dirfd, tmp_name, 0);
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    /* 目录项也要落盘，否则掉电后可能看不到新名字 */
    if (fsync(dirfd) != 0) {
        saved_errno = errno;
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    (void)close(dirfd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* pidfile 读取                                                        */
/* ------------------------------------------------------------------ */

int ota_sup_pidfile_read(
    const char *install_dir,
    ota_sup_identity_t *identity_out)
{
    char buffer[OTA_SUP_PIDFILE_MAX + 1];
    int dirfd;
    int fd;
    int saved_errno;
    ssize_t n;
    struct stat st;
    size_t total = 0;

    if (identity_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    dirfd = open_run_dir(install_dir, 0);

    if (dirfd < 0)
        return -1;   /* ENOENT：run 目录不存在，等价于从未启动 */

    /* O_NOFOLLOW：pidfile 不该是符号链接，若是则拒绝 */
    fd = openat(
        dirfd,
        OTA_SUP_PID_NAME,
        O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0) {
        saved_errno = errno;
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    (void)close(dirfd);

    if (fstat(fd, &st) != 0) {
        saved_errno = errno;
        (void)close(fd);
        errno = saved_errno;
        return -1;
    }

    if (!S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        st.st_size > OTA_SUP_PIDFILE_MAX) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }

    while (total < (size_t)st.st_size) {
        n = read(
            fd,
            buffer + total,
            (size_t)st.st_size - total);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            saved_errno = (n < 0) ? errno : EIO;
            (void)close(fd);
            errno = saved_errno;
            return -1;
        }

        total += (size_t)n;
    }

    (void)close(fd);

    buffer[total] = '\0';

    /*
     * 严格格式校验：只允许 "<pid> <starttime>\n"
     *
     * 不用 sscanf：%llu 溢出时 C 标准未定义，glibc 实际返回
     * ULLONG_MAX 且仍报告成功，于是 "12345 678999...999" 会被
     * 当成合法记录接受（本模块自测已捕获该缺陷）。
     * 这里改用 strtoull 显式检查 ERANGE。
     */
    const char *cursor = buffer;
    char *end = NULL;

    errno = 0;
    long long pid_value = strtoll(cursor, &end, 10);

    if (errno != 0 || end == cursor ||
        pid_value <= 0 || pid_value > INT_MAX) {
        errno = EINVAL;
        return -1;
    }

    cursor = end;

    /* pid 与 starttime 之间必须恰好一个空格 */
    if (*cursor != ' ') {
        errno = EINVAL;
        return -1;
    }

    cursor++;

    errno = 0;
    unsigned long long starttime_value =
        strtoull(cursor, &end, 10);

    if (errno != 0 || end == cursor || starttime_value == 0) {
        /* ERANGE 也归入此类：数值越界即视为损坏 */
        errno = EINVAL;
        return -1;
    }

    cursor = end;

    /* 剩余部分只允许空白（一个换行） */
    while (*cursor != '\0') {
        char c = *cursor;

        if (c != '\n' && c != ' ' && c != '\t' && c != '\r') {
            errno = EINVAL;
            return -1;
        }

        cursor++;
    }

    identity_out->pid = (pid_t)pid_value;
    identity_out->starttime = starttime_value;

    return 0;
}

int ota_sup_pidfile_remove(const char *install_dir)
{
    int dirfd;
    int saved_errno;

    dirfd = open_run_dir(install_dir, 0);

    if (dirfd < 0) {
        /* run 目录不存在 = 本来就没有 pidfile */
        if (errno == ENOENT)
            return 0;

        return -1;
    }

    if (unlinkat(dirfd, OTA_SUP_PID_NAME, 0) != 0 &&
        errno != ENOENT) {
        saved_errno = errno;
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    /*
     * 清理可能的残留临时文件。
     *
     * 临时文件名带 pid 后缀，无法在不知道 pid 的情况下精确删除，
     * 因此这里扫描 run 目录，删除所有匹配前缀的普通文件。
     * 只删前缀匹配项，不误删其他文件。
     */
    int scanfd = dup(dirfd);

    if (scanfd >= 0) {
        DIR *dir = fdopendir(scanfd);

        if (dir != NULL) {
            struct dirent *entry;

            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(
                        entry->d_name,
                        OTA_SUP_TEMP_PREFIX,
                        strlen(OTA_SUP_TEMP_PREFIX)) == 0) {
                    (void)unlinkat(
                        dirfd, entry->d_name, 0);
                }
            }

            (void)closedir(dir);
        } else {
            (void)close(scanfd);
        }
    }

    (void)close(dirfd);
    return 0;
}

/* ------------------------------------------------------------------ */
/* 身份校验与查询                                                      */
/* ------------------------------------------------------------------ */

ota_sup_state_t ota_sup_verify_identity(
    const ota_sup_identity_t *identity)
{
    unsigned long long current = 0;
    char process_state = '\0';

    if (identity == NULL)
        return OTA_SUP_UNKNOWN;

    /*
     * pid <= 1 一律 UNKNOWN。
     *
     * pid 1 是 init，pid 0 是进程组语义；对它们做任何操作都是错误。
     * 这里保守地拒绝判定，让调用方中止，而不是给出一个"可以操作"的结论。
     */
    if (identity->pid <= 1)
        return OTA_SUP_UNKNOWN;

    if (ota_sup_read_starttime(identity->pid, &current) != 0) {
        /* 进程确实不存在 */
        if (errno == ENOENT)
            return OTA_SUP_STOPPED;

        /* /proc 读不了、格式异常等：不敢断言 */
        return OTA_SUP_UNKNOWN;
    }

    /*
     * 僵尸检测 —— 这是 S1-R 修正的关键点。
     *
     * 僵尸进程（state='Z'）的 /proc 条目仍然存在，starttime 仍可读且
     * 与记录一致。如果只比对 starttime，会把它判为 RUNNING，
     * 而实际上该进程早已终止、不响应任何业务。
     *
     * 后果举例：OTA 想停旧版 -> query 返回 RUNNING ->
     * 发 SIGTERM 给僵尸（无效果）-> 等到超时 -> SIGKILL 同样无效
     * -> 卡在"停不下来"，或错误地认为旧版仍在服务。
     *
     * 因此：先看 state，再比 starttime。
     */
    if (ota_sup_read_state(identity->pid, &process_state) != 0) {
        if (errno == ENOENT)
            return OTA_SUP_STOPPED;

        return OTA_SUP_UNKNOWN;
    }

    if (!ota_sup_state_is_alive(process_state))
        return OTA_SUP_STOPPED;

    if (current == identity->starttime)
        return OTA_SUP_RUNNING;

    /*
     * pid 存在但 starttime 不同：pid 已被复用。
     * 原进程已退出，绝不能把新进程当成 Gateway 去管理。
     */
    return OTA_SUP_STOPPED;
}

ota_sup_state_t ota_sup_query(
    const char *install_dir,
    pid_t *pid_out)
{
    ota_sup_identity_t identity = {0, 0};

    if (pid_out != NULL)
        *pid_out = -1;

    if (install_dir == NULL || install_dir[0] == '\0')
        return OTA_SUP_UNKNOWN;

    if (ota_sup_pidfile_read(install_dir, &identity) != 0) {
        /*
         * 文件不存在：从未启动，或已正常清理。
         * 这是明确的"未运行"。
         */
        if (errno == ENOENT)
            return OTA_SUP_STOPPED;

        /*
         * 其他错误（内容损坏、符号链接、权限）：
         * 无法确定是否存在一个正在运行的 Gateway，
         * 保守返回 UNKNOWN，让调用方中止。
         */
        return OTA_SUP_UNKNOWN;
    }

    ota_sup_state_t state = ota_sup_verify_identity(&identity);

    if (state == OTA_SUP_RUNNING && pid_out != NULL)
        *pid_out = identity.pid;

    return state;
}

const char *ota_sup_state_name(ota_sup_state_t state)
{
    switch (state) {
    case OTA_SUP_RUNNING:
        return "RUNNING";
    case OTA_SUP_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* S2：自管 Supervisor                                                 */
/* ------------------------------------------------------------------ */

int ota_sup_start_managed(
    const char *install_dir,
    const char *executable,
    char *const argv[],
    ota_sup_managed_t *handle_out)
{
    pid_t pid = -1;
    ota_sup_identity_t identity = {0, 0};

    if (install_dir == NULL || install_dir[0] == '\0' ||
        executable == NULL || executable[0] == '\0' ||
        argv == NULL || handle_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    /*
     * 前置检查：如果已有一个"确认在运行"的 Gateway，
     * 不允许再起一个。第二实例很可能会因 7000 端口冲突而失败，
     * 但那时 pidfile 已被覆盖，原进程的记录就丢了。
     *
     * UNKNOWN 同样拒绝——状态不明时启动新进程，可能造成两个实例并存。
     */
    ota_sup_state_t existing = ota_sup_query(install_dir, NULL);

    if (existing == OTA_SUP_RUNNING) {
        errno = EALREADY;
        return -1;
    }

    if (existing == OTA_SUP_UNKNOWN) {
        errno = EBUSY;
        return -1;
    }

    /* STOPPED：可以启动 */

    if (ota_process_start(executable, argv, &pid) != 0)
        return -1;

    /*
     * 采集 starttime。
     *
     * 注意时序风险：若子进程启动后极快退出，这里可能读不到 stat。
     * 此时确实无法登记身份，必须杀掉并报错，而不是写一个
     * starttime=0 的无效记录。
     */
    if (ota_sup_capture_identity(pid, &identity) != 0) {
        int saved_errno = errno;

        /*
         * 子进程可能已经退出并成为僵尸，必须回收，
         * 否则会留下僵尸进程。
         */
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);

        errno = saved_errno;
        return -1;
    }

    if (ota_sup_pidfile_write(install_dir, &identity) != 0) {
        int saved_errno = errno;

        /*
         * 无法登记身份 = 无法管理。
         * 必须把刚启动的进程收掉，避免留下一个
         * "在跑但没有任何记录"的孤儿 Gateway。
         */
        (void)kill(pid, SIGTERM);
        (void)waitpid(pid, NULL, 0);

        errno = saved_errno;
        return -1;
    }

    handle_out->pid = pid;
    handle_out->starttime = identity.starttime;
    handle_out->owner_pid = getpid();

    return 0;
}

int ota_sup_stop_managed(
    const char *install_dir,
    const ota_sup_managed_t *handle,
    int timeout_ms,
    int *wait_status,
    long long *elapsed_ms)
{
    int status = 0;
    long long elapsed = 0;

    if (install_dir == NULL || install_dir[0] == '\0' ||
        handle == NULL ||
        handle->pid <= 1 ||
        handle->owner_pid != getpid()) {
        /*
         * 最后一条是关键：只有启动它的进程才有资格停止它。
         * 跨进程 stop 会导致 waitpid 返回 ECHILD，
         * 或者更糟——去操作一个根本不是自己子进程的 pid。
         */
        errno = EPERM;
        return -1;
    }

    int rc = ota_process_stop(
        handle->pid, timeout_ms, &status, &elapsed);

    if (wait_status != NULL)
        *wait_status = status;

    if (elapsed_ms != NULL)
        *elapsed_ms = elapsed;

    if (rc < 0)
        return -1;

    /*
     * 进程已确认退出（或在此之前已退出），才删除 pidfile。
     *
     * 不要无条件删除：若 stop 失败但进程仍在，删掉记录会让
     * 后续 query 返回 STOPPED，误导调用方去启动第二个实例。
     */
    if (ota_sup_pidfile_remove(install_dir) != 0) {
        /*
         * 进程已停但记录删不掉，属于部分失败。
         * 返回原始 rc 更能反映进程状态；调用方可通过
         * query 进一步判断。这里不掩盖进程已停止的事实。
         */
        return rc;
    }

    return rc;
}
