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
#include <time.h>
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
 * 一次读取 /proc/<pid>/stat，解析出 state 与 starttime。
 *
 * 这是唯一真正访问 /proc 的地方。
 * read_state / read_starttime / probe_process 都基于它，
 * 保证三者对同一份 stat 内容的解析逻辑完全一致，
 * 也避免调用方为取两个字段而读两次（产生快照错配窗口）。
 *
 * 关键：comm 可能含空格与 ')'，必须从**最后一个** ')' 之后开始。
 * 用 strchr 取第一个 ')' 或按空白分列，都会静默取到错误值。
 *
 * ')' 之后的 token 序列：state(第3) ppid(4) ... starttime(22)
 * 因此 starttime 是 ')' 之后的第 20 个 token（索引 19）。
 */
static int read_stat_fields(
    pid_t pid,
    char *state_out,
    unsigned long long *starttime_out)
{
    char path[64];
    char buffer[1024];
    int fd;
    ssize_t n;

    if (pid <= 0 ||
        (state_out == NULL && starttime_out == NULL)) {
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

    /* 第 3 字段：state（单个字符） */
    while (*cursor == ' ')
        cursor++;

    if (*cursor == '\0' || *cursor == '\n') {
        errno = EINVAL;
        return -1;
    }

    char parsed_state = *cursor;

    /* 继续前进到第 22 字段：starttime */
    int token_index = 0;
    unsigned long long parsed_starttime = 0;
    int have_starttime = 0;

    while (*cursor != '\0') {
        while (*cursor == ' ')
            cursor++;

        if (*cursor == '\0' || *cursor == '\n')
            break;

        if (token_index == 19) {
            char *end = NULL;

            errno = 0;

            parsed_starttime = strtoull(cursor, &end, 10);

            if (errno != 0 || end == cursor) {
                errno = EINVAL;
                return -1;
            }

            have_starttime = 1;
            break;
        }

        while (*cursor != ' ' && *cursor != '\0' &&
               *cursor != '\n')
            cursor++;

        token_index++;
    }

    /*
     * 只要调用方需要 starttime，就必须真的解析到它。
     * 字段数不足说明 /proc 格式与预期不符。
     */
    if (starttime_out != NULL && !have_starttime) {
        errno = EINVAL;
        return -1;
    }

    if (state_out != NULL)
        *state_out = parsed_state;

    if (starttime_out != NULL)
        *starttime_out = parsed_starttime;

    return 0;
}

/*
 * 读取 /proc/<pid>/stat 的进程状态字符（第 3 字段）。
 */
int ota_sup_read_state(
    pid_t pid,
    char *state_out)
{
    return read_stat_fields(pid, state_out, NULL);
}

int ota_sup_probe_process(
    pid_t pid,
    char *state_out,
    unsigned long long *starttime_out)
{
    return read_stat_fields(pid, state_out, starttime_out);
}

/*
 * 读取 /proc/sys/kernel/random/boot_id。
 *
 * 内核输出形如 "1f2e3d4c-5b6a-7890-abcd-ef0123456789\n"。
 * 去掉尾部空白后返回；空内容视为失败。
 */
int ota_sup_read_boot_id(
    char *output,
    size_t capacity)
{
    const char *path = "/proc/sys/kernel/random/boot_id";
    char buffer[OTA_SUP_BOOT_ID_MAX * 2];
    int fd;
    ssize_t n;

    if (output == NULL || capacity < 2) {
        errno = EINVAL;
        return -1;
    }

    fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return -1;

    do {
        n = read(fd, buffer, sizeof(buffer) - 1);
    } while (n < 0 && errno == EINTR);

    (void)close(fd);

    if (n <= 0) {
        errno = (n == 0) ? EIO : errno;
        return -1;
    }

    buffer[n] = '\0';

    /* 去掉尾部换行与空白 */
    while (n > 0) {
        char c = buffer[n - 1];

        if (c == '\n' || c == '\r' || c == ' ' || c == '\t') {
            buffer[--n] = '\0';
            continue;
        }

        break;
    }

    if (n == 0) {
        errno = EINVAL;
        return -1;
    }

    if ((size_t)n >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }

    memcpy(output, buffer, (size_t)n + 1);

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
    if (starttime_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    /* 复用单次读取核心，避免与 read_state 逻辑分叉 */
    return read_stat_fields(pid, NULL, starttime_out);
}

int ota_sup_capture_identity(
    pid_t pid,
    ota_sup_identity_t *identity_out)
{
    unsigned long long starttime = 0;
    char boot_id[OTA_SUP_BOOT_ID_MAX];
    ota_sup_identity_t result;

    if (pid <= 0 || identity_out == NULL) {
        errno = EINVAL;
        return -1;
    }

    if (ota_sup_read_starttime(pid, &starttime) != 0)
        return -1;

    result.pid = pid;
    result.starttime = starttime;
    memset(result.boot_id, 0, sizeof(result.boot_id));

    /*
     * boot_id 读取失败不视为致命错误。
     *
     * 极少数内核配置下 /proc/sys/kernel/random/boot_id 可能不可读。
     * 此时退化为 (pid, starttime) 校验——能力削弱但仍然可用。
     * 不在这里失败，是为了不让一个辅助维度阻断主流程；
     * 调用方可通过读取结果中的 boot_id 是否为空来判断。
     */
    if (ota_sup_read_boot_id(boot_id, sizeof(boot_id)) == 0) {
        memcpy(result.boot_id, boot_id, sizeof(result.boot_id));
    }

    *identity_out = result;

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

    /*
     * 格式："<pid> <starttime> <boot_id>\n"
     *
     * boot_id 为空时省略第三段，保持与旧格式兼容：
     *   旧："12345 678\n"        -> 仅 (pid, starttime) 校验
     *   新："12345 678 <uuid>\n" -> 额外校验启动周期
     *
     * 读取端两种都接受，因此升级不会因遗留 pidfile 而失败。
     */
    if (identity->boot_id[0] != '\0') {
        len = snprintf(
            payload, sizeof(payload),
            "%ld %llu %s\n",
            (long)identity->pid,
            identity->starttime,
            identity->boot_id);
    } else {
        len = snprintf(
            payload, sizeof(payload),
            "%ld %llu\n",
            (long)identity->pid,
            identity->starttime);
    }

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

    /*
     * 可选第三段：boot_id
     *
     * 旧格式 "pid starttime\n" 在 starttime 后直接是空白/结尾，
     * 新格式 "pid starttime boot_id\n" 中间有空格分隔。
     * 两种都必须接受，否则升级后遗留的旧 pidfile 会全部判为损坏。
     */
    memset(identity_out->boot_id, 0,
           sizeof(identity_out->boot_id));

    if (*cursor == ' ') {
        cursor++;

        const char *boot_start = cursor;

        while (*cursor != '\0' && *cursor != '\n' &&
               *cursor != '\r' && *cursor != ' ' &&
               *cursor != '\t') {
            cursor++;
        }

        size_t boot_len = (size_t)(cursor - boot_start);

        /*
         * boot_id 必须严格是内核格式：36 字符 UUID。
         *
         * 不加这一层校验会有一个危险的后果：
         * 若把 "12345 678 999" 这种内容当成合法记录（999 作 boot_id），
         * 那么 verify 时 boot_id 当然不匹配，于是判为 STOPPED ——
         * 而 pid 12345 可能真的有一个 Gateway 在跑。
         * 结果 OTA 会误以为"没在运行"并启动第二个实例。
         *
         * 正确的做法是把格式非法视为"记录损坏" -> UNKNOWN -> 中止操作。
         * 宁可停下，也不要基于一个看不懂的记录继续。
         */
        if (boot_len != 36) {
            errno = EINVAL;
            return -1;
        }

        for (size_t i = 0; i < boot_len; i++) {
            char c = boot_start[i];
            int sep_ok = (i == 8 || i == 13 || i == 18 || i == 23);

            if (sep_ok) {
                if (c != '-') {
                    errno = EINVAL;
                    return -1;
                }
                continue;
            }

            int is_hex =
                (c >= '0' && c <= '9') ||
                (c >= 'a' && c <= 'f') ||
                (c >= 'A' && c <= 'F');

            if (!is_hex) {
                errno = EINVAL;
                return -1;
            }
        }

        if (boot_len >= sizeof(identity_out->boot_id)) {
            errno = EINVAL;
            return -1;
        }

        memcpy(identity_out->boot_id, boot_start, boot_len);
        identity_out->boot_id[boot_len] = '\0';
    }

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

    /*
     * S2-R：单次读取同时取得 state 与 starttime。
     *
     * 原先分两次读 /proc/<pid>/stat，两次之间有窗口：
     *   读 starttime 时是旧进程 -> 旧进程退出、pid 被复用
     *   -> 读 state 时是新进程 -> 把旧 starttime 与新状态组合判断。
     * 那样得出的结论不对应任何真实快照。
     * 合并为一次读取后不存在这种错配。
     */
    if (ota_sup_probe_process(
            identity->pid, &process_state, &current) != 0) {
        if (errno == ENOENT)
            return OTA_SUP_STOPPED;

        /* /proc 读不了、格式异常等：不敢断言 */
        return OTA_SUP_UNKNOWN;
    }

    /*
     * 僵尸检测 —— S1-R 修正的要点。
     *
     * 僵尸进程（state='Z'）的 /proc 条目仍然存在，starttime 仍可读且
     * 与记录一致。如果只比对 starttime，会把它判为 RUNNING，
     * 而实际上该进程早已终止、不响应任何业务。
     *
     * 注意顺序：先看 state，再比 starttime。
     */
    if (!ota_sup_state_is_alive(process_state))
        return OTA_SUP_STOPPED;

    /* S2-R：跨重启身份校验（boot_id） */
    if (identity->boot_id[0] != '\0') {
        char current_boot_id[OTA_SUP_BOOT_ID_MAX];

        if (ota_sup_read_boot_id(
                current_boot_id, sizeof(current_boot_id)) != 0) {
            /*
             * 有 boot_id 记录但当前读不到：
             * 无法确认是否为同一启动周期，保守返回 UNKNOWN。
             */
            return OTA_SUP_UNKNOWN;
        }

        if (strcmp(identity->boot_id, current_boot_id) != 0) {
            /*
             * boot_id 不同 = 跨重启。
             *
             * 记录来自上一个启动周期，其中的 pid 在本周期毫无意义：
             * 即使 pid 恰好存在、starttime 恰好相同，那也只是巧合，
             * 绝不是当初记录的那个 Gateway 实例。
             */
            return OTA_SUP_STOPPED;
        }
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
    ota_sup_identity_t identity = { .pid = 0, .starttime = 0, .boot_id = {0} };

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

/*
 * 有界回收一个刚被终止的子进程。
 *
 * 为什么不能用 waitpid(pid, NULL, 0)：
 *   无界阻塞。若子进程处于 D 状态（不可中断睡眠，常见于
 *   NFS/块设备 I/O 卡死），SIGKILL 也无法让它退出，
 *   父进程会永久挂住——OTA Agent 就此卡死，比不回收更糟。
 *
 * 策略：轮询等待至多 timeout_ms；超时则放弃回收并返回 -1。
 * 遗留的僵尸会在 OTA 进程自身退出时由 init 接管，
 * 这远好于让整个升级流程挂死。
 *
 * 返回 0 已回收；1 超时未回收；-1 系统调用错误。
 */
static int reap_bounded(pid_t pid, int timeout_ms)
{
    const int step_ms = 20;
    int waited = 0;

    for (;;) {
        int status = 0;
        pid_t r;

        do {
            r = waitpid(pid, &status, WNOHANG);
        } while (r < 0 && errno == EINTR);

        if (r > 0)
            return 0;      /* 已回收 */

        if (r < 0)
            return (errno == ECHILD) ? 0 : -1;

        if (waited >= timeout_ms)
            return 1;      /* 超时，放弃 */

        struct timespec ts = {
            .tv_sec = 0,
            .tv_nsec = step_ms * 1000000L
        };

        while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
        }

        waited += step_ms;
    }
}

int ota_sup_start_managed(
    const char *install_dir,
    const char *executable,
    char *const argv[],
    ota_sup_managed_t *handle_out)
{
    pid_t pid = -1;
    ota_sup_identity_t identity = { .pid = 0, .starttime = 0, .boot_id = {0} };

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
         *
         * 用有界回收：若进程卡在 D 状态无法终止，
         * 不能让 OTA 永久挂在这里。
         */
        (void)kill(pid, SIGKILL);
        (void)reap_bounded(pid, 2000);

        errno = saved_errno;
        return -1;
    }

    if (ota_sup_pidfile_write(install_dir, &identity) != 0) {
        int saved_errno = errno;

        /*
         * 无法登记身份 = 无法管理。
         * 必须把刚启动的进程收掉，避免留下一个
         * "在跑但没有任何记录"的孤儿 Gateway。
         *
         * 同样使用有界回收，避免进程不响应信号时永久阻塞。
         */
        (void)kill(pid, SIGTERM);

        if (reap_bounded(pid, 1000) != 0) {
            /* SIGTERM 未生效，升级为 SIGKILL 再等一次 */
            (void)kill(pid, SIGKILL);
            (void)reap_bounded(pid, 2000);
        }

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
