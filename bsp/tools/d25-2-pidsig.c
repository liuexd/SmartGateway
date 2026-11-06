/*
 * D25-2 设计验证原型：pid + starttime 二元组校验
 *
 * 目的：
 *   在正式实现 ota_supervisor.c 之前，先用最小原型验证
 *   /proc/<pid>/stat 解析与 pid 复用检测逻辑的正确性。
 *
 * 本文件是**验证工具**，不是产品代码，不进入任何构建目标。
 * 它只依赖 POSIX 与 Linux /proc，ARM 与 x86 语义一致。
 *
 * 编译（宿主机）：
 *   gcc -std=c11 -O2 -Wall -Wextra -Werror -o /tmp/pidsig pidsig.c
 *
 * 编译（ARM）：
 *   arm-buildroot-linux-gnueabihf-gcc -std=c11 -O2 \
 *       -mcpu=cortex-a7 -mfloat-abi=hard -mfpu=neon-vfpv4 \
 *       --sysroot=<sysroot> -o pidsig pidsig.c
 *
 * 自测（不需要其它进程）：
 *   ./pidsig selftest
 *
 * 检视任意 pid：
 *   ./pidsig inspect <pid>
 */

#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 查询结果 */
enum {
    PIDSTATE_RUNNING = 0,   /* 存在且 starttime 与记录一致 */
    PIDSTATE_STOPPED = 1,   /* 不存在，或 starttime 不一致（pid 已复用） */
    PIDSTATE_UNKNOWN = 2    /* 无法判定，调用方必须当作"可能在运行" */
};

typedef struct {
    pid_t pid;
    unsigned long long starttime;
} pid_record_t;

/*
 * 从 /proc/<pid>/stat 读取 starttime（第 22 字段，0-based 索引 21）。
 *
 * 关键点：
 *   comm 字段（第 2 字段）可能包含空格和 ')'，因此**不能**按空白分列。
 *   必须从**最后一个** ')' 之后开始解析。
 *
 *   实测反例：comm = "a b (c) )" 时，
 *   awk '{print $22}' 会静默返回 0，而正确值是数千万量级。
 *   静默返回错误值比报错更危险，因为它会让校验看起来"只是不匹配"。
 *
 * 返回 0 成功，-1 失败（errno 有效或文件格式异常）。
 */
static int read_starttime(
    pid_t pid,
    unsigned long long *out)
{
    char path[64];
    char buffer[1024];
    int fd;
    ssize_t n;

    if (pid <= 0 || out == NULL) {
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

    n = read(fd, buffer, sizeof(buffer) - 1);
    (void)close(fd);

    if (n <= 0) {
        if (n == 0)
            errno = EIO;
        return -1;
    }

    buffer[n] = '\0';

    /*
     * 从最后一个 ')' 开始。不能用 strchr（第一个）。
     *
     * 格式：pid (comm) state ppid ...
     *       ')' 之后是一个空格，然后是 state（第 3 字段）。
     */
    const char *close_paren = strrchr(buffer, ')');

    if (close_paren == NULL || close_paren[1] == '\0') {
        errno = EINVAL;
        return -1;
    }

    const char *cursor = close_paren + 1;

    /*
     * 跳过 comm 之后的字段，直到第 22 字段。
     *
     * ')' 之后第一次按空白切分得到的 token 是**第 3 字段**（state）。
     * starttime 是第 22 字段，因此它是第 20 个 token（22 - 3 + 1）。
     */
    unsigned long long value = 0;
    int token_index = 0;

    while (*cursor != '\0') {
        while (*cursor == ' ')
            cursor++;

        if (*cursor == '\0' || *cursor == '\n')
            break;

        /*
         * token_index 从 0 开始，对应第 3 字段。
         * 第 22 字段 => token_index == 19。
         */
        if (token_index == 19) {
            char *end = NULL;

            errno = 0;

            value = strtoull(cursor, &end, 10);

            if (errno != 0 || end == cursor) {
                errno = EINVAL;
                return -1;
            }

            *out = value;
            return 0;
        }

        while (*cursor != ' ' && *cursor != '\0' &&
               *cursor != '\n')
            cursor++;

        token_index++;
    }

    /* 字段数不足，说明 /proc 格式不符合预期 */
    errno = EINVAL;
    return -1;
}

/*
 * 校验 (pid, starttime) 是否仍指向同一个进程实例。
 *
 * 这是防误杀的核心：
 *   - 进程不存在        -> STOPPED
 *   - 存在但 starttime 不同 -> STOPPED（pid 被复用）
 *   - 读取失败/格式异常 -> UNKNOWN
 */
static int verify_record(const pid_record_t *record)
{
    unsigned long long current = 0;

    if (record == NULL || record->pid <= 1) {
        /* pid <= 1 永不视为可操作目标 */
        return PIDSTATE_UNKNOWN;
    }

    if (read_starttime(record->pid, &current) != 0) {
        if (errno == ENOENT)
            return PIDSTATE_STOPPED;

        return PIDSTATE_UNKNOWN;
    }

    return current == record->starttime
        ? PIDSTATE_RUNNING
        : PIDSTATE_STOPPED;
}

static const char *state_name(int state)
{
    switch (state) {
    case PIDSTATE_RUNNING:
        return "RUNNING";
    case PIDSTATE_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

/* ------------------------------------------------------------------ */
/* 自测                                                               */
/* ------------------------------------------------------------------ */

static int failures;

static void expect(
    const char *label,
    int actual,
    int expected)
{
    int ok = (actual == expected);

    printf(
        "  %-52s %-9s %s\n",
        label,
        state_name(actual),
        ok ? "PASS" : "FAIL");

    if (!ok) {
        printf(
            "        期望 %s\n",
            state_name(expected));
        failures++;
    }
}

static pid_t spawn_child(void)
{
    pid_t pid = fork();

    if (pid == 0) {
        /* 子进程：静默驻留 */
        for (;;)
            pause();
        _exit(0);
    }

    return pid;
}

static void reap(pid_t pid)
{
    if (pid > 1) {
        (void)kill(pid, SIGKILL);
        (void)waitpid(pid, NULL, 0);
    }
}

static int selftest(void)
{
    printf("== 1. 当前进程（pid=%ld）==\n", (long)getpid());

    pid_record_t self = { .pid = getpid(), .starttime = 0 };

    if (read_starttime(getpid(), &self.starttime) != 0) {
        printf("  FAIL: 无法读取自身 starttime\n");
        return 1;
    }

    printf("  starttime = %llu\n", self.starttime);

    if (self.starttime == 0) {
        printf(
            "  FAIL: starttime 为 0，解析可能取错了字段\n");
        failures++;
    } else {
        printf(
            "  %-52s %-9s PASS\n",
            "starttime 非 0（字段定位正确）", "OK");
    }

    printf("\n== 2. 存活的子进程 ==\n");

    pid_t child = spawn_child();

    if (child <= 0) {
        printf("  FAIL: fork\n");
        return 1;
    }

    pid_record_t live = { .pid = child, .starttime = 0 };

    if (read_starttime(child, &live.starttime) != 0) {
        printf("  FAIL: 无法读取子进程 starttime\n");
        reap(child);
        return 1;
    }

    expect("存活子进程 -> RUNNING",
           verify_record(&live), PIDSTATE_RUNNING);

    printf("\n== 3. pid 复用（starttime 被篡改）==\n");

    pid_record_t stale = live;

    /* 模拟 pid 被复用：pid 相同但 starttime 不同 */
    stale.starttime = live.starttime + 12345;

    expect("starttime 不符 -> STOPPED",
           verify_record(&stale), PIDSTATE_STOPPED);

    printf("\n== 4. 已退出进程 ==\n");

    reap(child);

    expect("已 reap 的子进程 -> STOPPED",
           verify_record(&live), PIDSTATE_STOPPED);

    printf("\n== 5. 保护性用例 ==\n");

    pid_record_t init_like = { .pid = 1, .starttime = 1 };

    expect("pid=1 -> UNKNOWN（绝不发信号）",
           verify_record(&init_like), PIDSTATE_UNKNOWN);

    pid_record_t invalid = { .pid = 0, .starttime = 0 };

    expect("pid=0 -> UNKNOWN",
           verify_record(&invalid), PIDSTATE_UNKNOWN);

    printf("\n== 6. comm 含空格与括号（解析陷阱）==\n");

    pid_t tricky = fork();

    if (tricky == 0) {
        prctl(PR_SET_NAME, "a b (c) )", 0, 0, 0);

        for (;;)
            pause();
        _exit(0);
    }

    if (tricky > 0) {
        /* 等子进程完成 prctl */
        struct timespec d = { .tv_sec = 0, .tv_nsec = 100000000 };
        nanosleep(&d, NULL);

        unsigned long long tricky_start = 0;

        if (read_starttime(tricky, &tricky_start) != 0) {
            printf("  FAIL: 无法解析特殊 comm 的 stat\n");
            failures++;
        } else if (tricky_start == 0) {
            printf(
                "  FAIL: starttime=0，说明取错了字段"
                "（正是 awk $22 的缺陷）\n");
            failures++;
        } else {
            printf(
                "  %-52s %-9s PASS\n",
                "comm='a b (c) )' 仍能正确解析", "OK");
            printf(
                "        解析值 = %llu\n", tricky_start);
        }

        pid_record_t rec = {
            .pid = tricky,
            .starttime = tricky_start
        };

        expect("特殊 comm 进程 -> RUNNING",
               verify_record(&rec), PIDSTATE_RUNNING);

        reap(tricky);
    }

    printf("\n== 7. starttime 精度（同批 fork）==\n");

    unsigned long long a = 0, b = 0;
    pid_t p1 = spawn_child();
    pid_t p2 = spawn_child();

    (void)read_starttime(p1, &a);
    (void)read_starttime(p2, &b);

    if (p1 > 0 && p2 > 0) {
        printf(
            "  pid %ld starttime=%llu\n", (long)p1, a);
        printf(
            "  pid %ld starttime=%llu\n", (long)p2, b);
        printf(
            "  %s\n",
            a == b
                ? "  两者相同 → 证实 starttime 不是唯一标识，"
                  "必须与 pid 组成二元组"
                : "  两者不同 → 本机未复现，但仍应按二元组实现");
    }

    reap(p1);
    reap(p2);

    printf("\n");

    if (failures != 0) {
        printf("存在 %d 个失败项\n", failures);
        return 1;
    }

    printf("全部自测通过\n");
    return 0;
}

static int inspect(pid_t pid)
{
    unsigned long long start = 0;

    if (read_starttime(pid, &start) != 0) {
        printf(
            "pid %ld: 读取失败 (%s)\n",
            (long)pid, strerror(errno));

        return errno == ENOENT ? 1 : 2;
    }

    printf(
        "pid %ld: starttime = %llu\n",
        (long)pid, start);

    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "selftest") == 0)
        return selftest();

    if (argc == 3 && strcmp(argv[1], "inspect") == 0) {
        char *end = NULL;
        long pid = strtol(argv[2], &end, 10);

        if (end == argv[2] || *end != '\0' || pid <= 0) {
            fprintf(stderr, "无效 pid: %s\n", argv[2]);
            return 2;
        }

        return inspect((pid_t)pid);
    }

    fprintf(stderr,
            "用法:\n"
            "  %s selftest\n"
            "  %s inspect <pid>\n",
            argv[0], argv[0]);

    return 2;
}
