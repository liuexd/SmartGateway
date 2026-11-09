#define _POSIX_C_SOURCE 200809L

#include "ota_supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define OTA_SUP_RUN_DIR "run"
#define OTA_SUP_PID_NAME "gateway.pid"
#define OTA_SUP_TEMP_NAME "gateway.pid.tmp"

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

    tmpfd = openat(
        dirfd,
        OTA_SUP_TEMP_NAME,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW,
        0600);

    if (tmpfd < 0) {
        saved_errno = errno;
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    /* 复用已有临时文件时，确保权限仍是 0600 */
    if (fstat(tmpfd, &st) != 0 || !S_ISREG(st.st_mode) ||
        (st.st_mode & 077) != 0) {
        if (fchmod(tmpfd, 0600) != 0) {
            saved_errno = errno;
            (void)close(tmpfd);
            (void)unlinkat(dirfd, OTA_SUP_TEMP_NAME, 0);
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
        (void)unlinkat(dirfd, OTA_SUP_TEMP_NAME, 0);
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
            (void)unlinkat(dirfd, OTA_SUP_TEMP_NAME, 0);
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
        (void)unlinkat(dirfd, OTA_SUP_TEMP_NAME, 0);
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    if (close(tmpfd) != 0) {
        saved_errno = errno;
        (void)unlinkat(dirfd, OTA_SUP_TEMP_NAME, 0);
        (void)close(dirfd);
        errno = saved_errno;
        return -1;
    }

    if (renameat(
            dirfd, OTA_SUP_TEMP_NAME,
            dirfd, OTA_SUP_PID_NAME) != 0) {
        saved_errno = errno;
        (void)unlinkat(dirfd, OTA_SUP_TEMP_NAME, 0);
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

    /* 临时文件若残留也一并清掉 */
    (void)unlinkat(dirfd, OTA_SUP_TEMP_NAME, 0);

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
