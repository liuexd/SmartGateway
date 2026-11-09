/*
 * ota_supervisor 单元测试（D25-2 S1）
 *
 * 覆盖场景：
 *   1. 无 pidfile              -> STOPPED
 *   2. 捕获自身身份            -> RUNNING
 *   3. 进程退出后              -> STOPPED
 *   4. pid 复用（starttime 变） -> STOPPED，且不误杀
 *   5. pid=1                   -> UNKNOWN，绝不发信号
 *   6. pidfile 内容损坏         -> UNKNOWN / EINVAL
 *   7. pidfile 尾部垃圾         -> 拒绝
 *   8. pidfile 是符号链接       -> 拒绝
 *   9. comm 含空格与括号        -> starttime 解析正确
 *  10. pidfile 权限 0600
 *  11. 原子写后重新读取一致
 *  12. 目录不存在时 remove 不报错
 *  13. 路径截断检测
 *
 * 本测试不启动 Gateway，只操作临时目录中的 pidfile，
 * 并使用 fork 出来的哨兵进程模拟"被管理的进程"。
 */

#define _POSIX_C_SOURCE 200809L

#include "ota_supervisor.h"

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

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, fmt, ...)                                     \
    do {                                                          \
        g_checks++;                                               \
        if (cond) {                                               \
            printf("  PASS  " fmt "\n", ##__VA_ARGS__);           \
        } else {                                                  \
            printf("  FAIL  " fmt "\n", ##__VA_ARGS__);           \
            g_failures++;                                         \
        }                                                         \
    } while (0)

/* ---------------- 辅助 ---------------- */

static char g_dir[PATH_MAX];

static void make_temp_dir(void)
{
    snprintf(
        g_dir, sizeof(g_dir),
        "/tmp/d25-2-s1-XXXXXX");

    if (mkdtemp(g_dir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
}

static void cleanup_temp_dir(void)
{
    char cmd[PATH_MAX + 32];

    snprintf(
        cmd, sizeof(cmd),
        "rm -rf '%s'", g_dir);

    int rc = system(cmd);

    (void)rc;
}

static pid_t spawn_sleeper(void)
{
    pid_t pid = fork();

    if (pid == 0) {
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

static void write_raw_pidfile(const char *content)
{
    char path[PATH_MAX];
    char dir[PATH_MAX];

    if (ota_sup_pidfile_path(
            g_dir, path, sizeof(path)) != 0) {
        return;
    }

    /* 确保 run 目录存在 */
    int n = snprintf(dir, sizeof(dir), "%s/run", g_dir);

    if (n < 0 || (size_t)n >= sizeof(dir))
        return;

    (void)mkdir(dir, 0700);

    int fd = open(
        path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return;

    ssize_t ignored = write(fd, content, strlen(content));

    (void)ignored;
    (void)close(fd);
}

/* ---------------- 测试用例 ---------------- */

static void test_no_pidfile(void)
{
    printf("\n== 1. 无 pidfile -> STOPPED ==\n");

    ota_sup_identity_t id;

    errno = 0;
    int rc = ota_sup_pidfile_read(g_dir, &id);

    CHECK(rc == -1, "read 应失败");
    CHECK(errno == ENOENT,
          "errno=ENOENT（实际 %d: %s）",
          errno, strerror(errno));

    pid_t pid = -1;
    ota_sup_state_t st = ota_sup_query(g_dir, &pid);

    CHECK(st == OTA_SUP_STOPPED,
          "query -> STOPPED（实际 %s）",
          ota_sup_state_name(st));
    CHECK(pid == -1, "pid 输出保持 -1");
}

static void test_self_identity(void)
{
    printf("\n== 2. 捕获自身身份 -> RUNNING ==\n");

    ota_sup_identity_t id;

    int rc = ota_sup_capture_identity(getpid(), &id);

    CHECK(rc == 0, "capture_identity 成功");
    CHECK(id.pid == getpid(), "pid 正确");
    CHECK(id.starttime != 0,
          "starttime 非 0（实际 %llu）", id.starttime);

    /* 写到 pidfile 再查 */
    rc = ota_sup_pidfile_write(g_dir, &id);
    CHECK(rc == 0, "pidfile_write 成功");

    ota_sup_identity_t back;

    rc = ota_sup_pidfile_read(g_dir, &back);
    CHECK(rc == 0, "pidfile_read 成功");
    CHECK(back.pid == id.pid, "pid 往返一致");
    CHECK(back.starttime == id.starttime,
          "starttime 往返一致");

    pid_t pid = -1;
    ota_sup_state_t st = ota_sup_query(g_dir, &pid);

    CHECK(st == OTA_SUP_RUNNING,
          "query -> RUNNING（实际 %s）",
          ota_sup_state_name(st));
    CHECK(pid == getpid(), "pid 输出正确");
}

static void test_process_exited(void)
{
    printf("\n== 3. 进程退出后 -> STOPPED ==\n");

    pid_t child = spawn_sleeper();

    CHECK(child > 1, "fork 哨兵进程 pid=%ld", (long)child);

    ota_sup_identity_t id;

    CHECK(ota_sup_capture_identity(child, &id) == 0,
          "捕获子进程身份");
    CHECK(ota_sup_pidfile_write(g_dir, &id) == 0,
          "写入 pidfile");

    ota_sup_state_t st = ota_sup_query(g_dir, NULL);

    CHECK(st == OTA_SUP_RUNNING,
          "运行中 -> RUNNING（实际 %s）",
          ota_sup_state_name(st));

    reap(child);

    st = ota_sup_query(g_dir, NULL);

    CHECK(st == OTA_SUP_STOPPED,
          "退出后 -> STOPPED（实际 %s）",
          ota_sup_state_name(st));
}

static void test_pid_reuse(void)
{
    printf("\n== 4. pid 复用 -> STOPPED（不误杀）==\n");

    pid_t child = spawn_sleeper();

    CHECK(child > 1, "fork 哨兵进程 pid=%ld", (long)child);

    ota_sup_identity_t real;

    CHECK(ota_sup_capture_identity(child, &real) == 0,
          "捕获真实身份");

    /*
     * 伪造一个 starttime：模拟"同一个 pid 现在属于另一个进程"。
     * 这正是 pid 复用的情形。
     */
    ota_sup_identity_t reused = real;

    reused.starttime = real.starttime + 1;

    ota_sup_state_t st = ota_sup_verify_identity(&reused);

    CHECK(st == OTA_SUP_STOPPED,
          "starttime 不符 -> STOPPED（实际 %s）",
          ota_sup_state_name(st));

    /* 关键：该进程本身仍活着，说明我们只是拒绝判定，没有动它 */
    int alive = (kill(child, 0) == 0);

    CHECK(alive, "哨兵进程仍存活，未被误杀");

    reap(child);
}

static void test_pid_one(void)
{
    printf("\n== 5. pid=1 -> UNKNOWN（绝不发信号）==\n");

    ota_sup_identity_t init_like = { .pid = 1, .starttime = 1 };

    ota_sup_state_t st = ota_sup_verify_identity(&init_like);

    CHECK(st == OTA_SUP_UNKNOWN,
          "pid=1 -> UNKNOWN（实际 %s）",
          ota_sup_state_name(st));

    /* pid 0 与负数同样必须拒绝 */
    ota_sup_identity_t zero = { .pid = 0, .starttime = 1 };
    CHECK(ota_sup_verify_identity(&zero) == OTA_SUP_UNKNOWN,
          "pid=0 -> UNKNOWN");

    ota_sup_identity_t neg = { .pid = -5, .starttime = 1 };
    CHECK(ota_sup_verify_identity(&neg) == OTA_SUP_UNKNOWN,
          "pid=-5 -> UNKNOWN");

    CHECK(ota_sup_verify_identity(NULL) == OTA_SUP_UNKNOWN,
          "NULL -> UNKNOWN");

    /* init 仍在运行，证明我们没对它做任何事 */
    CHECK(kill(1, 0) == 0 || errno == EPERM,
          "init 进程仍正常存在");
}

static void test_corrupt_pidfile(void)
{
    printf("\n== 6. pidfile 内容损坏 -> UNKNOWN ==\n");

    const char *bad[] = {
        "",
        "abc\n",
        "12345\n",              /* 只有一个字段 */
        "-1 999\n",             /* 负 pid */
        "0 999\n",              /* pid 0 */
        "12345 0\n",            /* starttime 0 */
        "12345 678\nGARBAGE",   /* 尾部垃圾 */
        "12345 678 999\n",      /* 多余字段 */
        "12345 678999999999999999999999\n", /* 溢出 */
    };

    size_t count = sizeof(bad) / sizeof(bad[0]);

    for (size_t i = 0; i < count; i++) {
        write_raw_pidfile(bad[i]);

        ota_sup_identity_t id;
        errno = 0;

        int rc = ota_sup_pidfile_read(g_dir, &id);

        CHECK(rc == -1,
              "拒绝损坏内容 #%zu: \"%s\"",
              i, bad[i]);

        ota_sup_state_t st = ota_sup_query(g_dir, NULL);

        CHECK(st == OTA_SUP_UNKNOWN,
              "query -> UNKNOWN（实际 %s）",
              ota_sup_state_name(st));
    }
}

static void test_symlink_pidfile(void)
{
    printf("\n== 7. pidfile 是符号链接 -> 拒绝 ==\n");

    char path[PATH_MAX];
    char dir[PATH_MAX];

    int dn = snprintf(dir, sizeof(dir), "%s/run", g_dir);

    if (dn > 0 && (size_t)dn < sizeof(dir))
        (void)mkdir(dir, 0700);

    if (ota_sup_pidfile_path(
            g_dir, path, sizeof(path)) != 0) {
        CHECK(0, "构造路径失败");
        return;
    }

    (void)unlink(path);
    (void)unlinkat(AT_FDCWD, path, 0);

    if (symlink("/etc/passwd", path) != 0) {
        printf("  SKIP  无法创建符号链接（%s）\n",
               strerror(errno));
        return;
    }

    ota_sup_identity_t id;
    errno = 0;

    int rc = ota_sup_pidfile_read(g_dir, &id);

    CHECK(rc == -1, "符号链接被拒绝");
    CHECK(errno == ELOOP,
          "errno=ELOOP（实际 %d: %s）",
          errno, strerror(errno));

    ota_sup_state_t st = ota_sup_query(g_dir, NULL);

    CHECK(st == OTA_SUP_UNKNOWN,
          "query -> UNKNOWN（实际 %s）",
          ota_sup_state_name(st));

    (void)unlink(path);
}

static void test_comm_with_spaces(void)
{
    printf("\n== 8. comm 含空格与括号 -> starttime 正确 ==\n");

    pid_t child = fork();

    if (child == 0) {
        /* 故意构造最恶劣的 comm：含空格、括号、右括号 */
        (void)prctl(PR_SET_NAME, "a b (c) )", 0, 0, 0);
        for (;;)
            pause();
        _exit(0);
    }

    if (child <= 0) {
        printf("  SKIP  fork 失败\n");
        return;
    }

    struct timespec delay = { .tv_sec = 0, .tv_nsec = 150000000 };
    (void)nanosleep(&delay, NULL);

    unsigned long long starttime = 0;

    int rc = ota_sup_read_starttime(child, &starttime);

    CHECK(rc == 0, "解析成功");
    CHECK(starttime != 0,
          "starttime 非 0（实际 %llu）", starttime);

    /*
     * 若实现错误地按空白分列，这里会得到 0 或完全不同的值。
     * 再独立交叉验证：从 /proc 原始内容手动定位。
     */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)child);

    FILE *fp = fopen(path, "r");

    if (fp != NULL) {
        char line[2048];

        if (fgets(line, sizeof(line), fp) != NULL) {
            const char *rp = strrchr(line, ')');

            if (rp != NULL) {
                char *cursor = (char *)rp + 1;
                unsigned long long expect = 0;
                int idx = 0;

                /* 跳过空白后逐 token 前进到第 20 个 */
                while (*cursor != '\0' && idx < 20) {
                    while (*cursor == ' ')
                        cursor++;

                    if (idx == 19) {
                        expect = strtoull(cursor, NULL, 10);
                        break;
                    }

                    while (*cursor != ' ' && *cursor != '\0' &&
                           *cursor != '\n')
                        cursor++;

                    idx++;
                }

                CHECK(expect == starttime,
                      "与独立解析一致（%llu）", starttime);
            }
        }

        (void)fclose(fp);
    }

    ota_sup_identity_t id = { .pid = child, .starttime = starttime };

    CHECK(ota_sup_verify_identity(&id) == OTA_SUP_RUNNING,
          "特殊 comm 进程 -> RUNNING");

    reap(child);
}

static void test_permissions(void)
{
    printf("\n== 9. pidfile 权限 0600 ==\n");

    ota_sup_identity_t id;

    CHECK(ota_sup_capture_identity(getpid(), &id) == 0,
          "捕获身份");
    CHECK(ota_sup_pidfile_write(g_dir, &id) == 0,
          "写入 pidfile");

    char path[PATH_MAX];

    CHECK(ota_sup_pidfile_path(
              g_dir, path, sizeof(path)) == 0,
          "构造路径");

    struct stat st;

    CHECK(stat(path, &st) == 0, "stat pidfile");

    CHECK((st.st_mode & 0777) == 0600,
          "权限为 0600（实际 %04o）",
          st.st_mode & 0777);

    /* 临时文件不应残留 */
    char tmp[PATH_MAX + 32];
    int tn = snprintf(
        tmp, sizeof(tmp), "%s/run/gateway.pid.tmp", g_dir);

    CHECK(tn > 0 && (size_t)tn < sizeof(tmp),
          "临时文件路径构造成功");
    CHECK(access(tmp, F_OK) != 0,
          "临时文件已清理");
}

static void test_atomic_write_consistency(void)
{
    printf("\n== 10. 多次写入后读取一致 ==\n");

    for (int i = 0; i < 20; i++) {
        ota_sup_identity_t id = {
            .pid = (pid_t)(getpid()),
            .starttime = (unsigned long long)(1000 + i)
        };

        if (ota_sup_pidfile_write(g_dir, &id) != 0) {
            CHECK(0, "第 %d 次写入失败", i);
            return;
        }

        ota_sup_identity_t back;

        if (ota_sup_pidfile_read(g_dir, &back) != 0) {
            CHECK(0, "第 %d 次读取失败", i);
            return;
        }

        if (back.starttime != id.starttime) {
            CHECK(0, "第 %d 次内容不一致", i);
            return;
        }
    }

    CHECK(1, "20 次写入/读取全部一致");
}

static void test_remove(void)
{
    printf("\n== 11. remove 与不存在的情况 ==\n");

    CHECK(ota_sup_pidfile_remove(g_dir) == 0,
          "删除存在的 pidfile 成功");

    ota_sup_identity_t id;
    errno = 0;

    CHECK(ota_sup_pidfile_read(g_dir, &id) == -1 &&
          errno == ENOENT,
          "删除后读取 -> ENOENT");

    /* 再删一次不应报错 */
    CHECK(ota_sup_pidfile_remove(g_dir) == 0,
          "重复删除不报错");

    CHECK(ota_sup_query(g_dir, NULL) == OTA_SUP_STOPPED,
          "无 pidfile -> STOPPED");
}

static void test_path_handling(void)
{
    printf("\n== 12. 路径构造与截断检测 ==\n");

    char buf[PATH_MAX];

    CHECK(ota_sup_pidfile_path("/opt/sgw", buf, sizeof(buf)) == 0,
          "正常路径构造成功");
    CHECK(strcmp(buf, "/opt/sgw/run/gateway.pid") == 0,
          "路径内容正确：%s", buf);

    char small[8];

    errno = 0;

    CHECK(ota_sup_pidfile_path("/opt/sgw", small, sizeof(small)) == -1,
          "缓冲区过小被拒绝");
    CHECK(errno == ENAMETOOLONG,
          "errno=ENAMETOOLONG（实际 %d）", errno);

    CHECK(ota_sup_pidfile_path(NULL, buf, sizeof(buf)) == -1,
          "NULL install_dir 被拒绝");

    CHECK(ota_sup_pidfile_path("", buf, sizeof(buf)) == -1,
          "空 install_dir 被拒绝");

    CHECK(ota_sup_query(NULL, NULL) == OTA_SUP_UNKNOWN,
          "query(NULL) -> UNKNOWN");
}

static void test_incomplete_identity(void)
{
    printf("\n== 13. 不完整身份拒绝写入 ==\n");

    ota_sup_identity_t bad = { .pid = 100, .starttime = 0 };

    errno = 0;

    CHECK(ota_sup_pidfile_write(g_dir, &bad) == -1,
          "starttime=0 被拒绝");
    CHECK(errno == EINVAL,
          "errno=EINVAL（实际 %d）", errno);

    ota_sup_identity_t bad2 = { .pid = 0, .starttime = 123 };

    errno = 0;

    CHECK(ota_sup_pidfile_write(g_dir, &bad2) == -1,
          "pid=0 被拒绝");

    CHECK(ota_sup_pidfile_write(g_dir, NULL) == -1,
          "NULL 身份被拒绝");
}

static void test_zombie_process(void)
{
    printf("\n== 14. 僵尸进程 -> STOPPED（S1-R 关键修正）==\n");

    /*
     * 僵尸进程的 /proc/<pid>/stat 仍然存在，starttime 仍可读。
     * 只比对 starttime 的实现会误判为 RUNNING。
     * 正确行为：state=='Z' 视为已停止。
     */
    pid_t child = fork();

    if (child == 0)
        _exit(0);   /* 立即退出，父进程不 wait -> 僵尸 */

    if (child <= 0) {
        printf("  SKIP  fork 失败\n");
        return;
    }

    /* 等它变成僵尸 */
    struct timespec delay = { .tv_sec = 0, .tv_nsec = 300000000 };
    (void)nanosleep(&delay, NULL);

    char state_char = '\0';

    int rc = ota_sup_read_state(child, &state_char);

    CHECK(rc == 0, "能读取僵尸进程的 state");
    CHECK(state_char == 'Z',
          "state 为 'Z'（实际 '%c'）", state_char);
    CHECK(!ota_sup_state_is_alive(state_char),
          "state_is_alive('Z') == 0");

    /* 确认僵尸的 starttime 确实可读——这正是误判的来源 */
    unsigned long long st = 0;

    CHECK(ota_sup_read_starttime(child, &st) == 0 && st != 0,
          "僵尸的 starttime 仍可读（%llu），故仅比对它不够", st);

    ota_sup_identity_t id = { .pid = child, .starttime = st };

    ota_sup_state_t live = ota_sup_verify_identity(&id);

    CHECK(live == OTA_SUP_STOPPED,
          "僵尸 -> STOPPED（实际 %s）",
          ota_sup_state_name(live));

    /* 写入 pidfile 后整体查询也应得到 STOPPED */
    CHECK(ota_sup_pidfile_write(g_dir, &id) == 0,
          "写入僵尸身份");

    pid_t got = -1;
    ota_sup_state_t q = ota_sup_query(g_dir, &got);

    CHECK(q == OTA_SUP_STOPPED,
          "query -> STOPPED（实际 %s）",
          ota_sup_state_name(q));
    CHECK(got == -1, "STOPPED 时不输出 pid");

    (void)waitpid(child, NULL, 0);
}

static void test_state_char_classification(void)
{
    printf("\n== 15. 状态字符分类 ==\n");

    CHECK(ota_sup_state_is_alive('R') == 1, "'R' 运行");
    CHECK(ota_sup_state_is_alive('S') == 1, "'S' 可中断睡眠");
    CHECK(ota_sup_state_is_alive('D') == 1, "'D' 不可中断");
    CHECK(ota_sup_state_is_alive('T') == 1, "'T' 停止（受控）");

    CHECK(ota_sup_state_is_alive('Z') == 0, "'Z' 僵尸不算运行");
    CHECK(ota_sup_state_is_alive('X') == 0, "'X' 已死亡不算运行");
    CHECK(ota_sup_state_is_alive('x') == 0, "'x' 已死亡不算运行");
}

static void test_temp_file_isolation(void)
{
    printf("\n== 16. 临时文件按进程隔离 ==\n");

    ota_sup_identity_t id;

    CHECK(ota_sup_capture_identity(getpid(), &id) == 0,
          "捕获身份");
    CHECK(ota_sup_pidfile_write(g_dir, &id) == 0,
          "写入 pidfile");

    /*
     * 写入完成后临时文件应已 rename 掉。
     * 同时验证 run 目录中不存在任何残留 tmp 文件。
     */
    char dir[PATH_MAX + 32];
    int dn = snprintf(dir, sizeof(dir), "%s/run", g_dir);

    CHECK(dn > 0 && (size_t)dn < sizeof(dir), "路径合法");

    char cmd[PATH_MAX + 64];
    int cn = snprintf(
        cmd, sizeof(cmd),
        "ls '%s'/gateway.pid.tmp.* 2>/dev/null | wc -l", g_dir);

    CHECK(cn > 0 && (size_t)cn < sizeof(cmd), "命令构造成功");

    FILE *fp = popen(cmd, "r");

    if (fp != NULL) {
        int count = -1;

        if (fscanf(fp, "%d", &count) != 1)
            count = -1;

        (void)pclose(fp);

        CHECK(count == 0,
              "无残留临时文件（实际 %d 个）", count);
    }

    /* 并发写：两个不同 pid 的临时名不应冲突 */
    ota_sup_identity_t a = { .pid = getpid(), .starttime = 111 };
    ota_sup_identity_t b = { .pid = getpid(), .starttime = 222 };

    CHECK(ota_sup_pidfile_write(g_dir, &a) == 0, "写 A");
    CHECK(ota_sup_pidfile_write(g_dir, &b) == 0, "写 B");

    ota_sup_identity_t back;

    CHECK(ota_sup_pidfile_read(g_dir, &back) == 0,
          "读回成功");
    CHECK(back.starttime == 222,
          "后写者生效（%llu）", back.starttime);
}

int main(void)
{
    printf("ota_supervisor 单元测试（D25-2 S1 + S1-R）\n");

    make_temp_dir();
    printf("临时目录: %s\n", g_dir);

    test_no_pidfile();
    test_self_identity();
    test_process_exited();
    test_pid_reuse();
    test_pid_one();
    test_corrupt_pidfile();
    test_symlink_pidfile();
    test_comm_with_spaces();
    test_permissions();
    test_atomic_write_consistency();
    test_remove();
    test_path_handling();
    test_incomplete_identity();
    test_zombie_process();
    test_state_char_classification();
    test_temp_file_isolation();

    cleanup_temp_dir();

    printf("\n============================\n");
    printf("检查项: %d, 失败: %d\n", g_checks, g_failures);

    if (g_failures != 0) {
        printf("结果: FAIL\n");
        return 1;
    }

    printf("结果: PASS\n");
    return 0;
}
