/*
 * ota_supervisor 单元测试（D25-2 S1 + S1-R + S2-R）
 *
 * 覆盖场景（与 main() 中实际调用一一对应）：
 *   1.  无 pidfile                  -> STOPPED
 *   2.  捕获自身身份                -> RUNNING
 *   3.  进程退出后                  -> STOPPED
 *   4.  pid 复用（starttime 变）     -> STOPPED，且不误杀
 *   5.  pid=1                       -> UNKNOWN，绝不发信号
 *   6.  pidfile 内容损坏             -> UNKNOWN / EINVAL
 *   7.  pidfile 是符号链接           -> 拒绝
 *   8.  comm 含空格与括号            -> starttime 解析正确
 *   9.  pidfile 权限 0600
 *  10.  原子写后重新读取一致
 *  11.  目录不存在时 remove 不报错
 *  12.  路径截断检测
 *  13.  不完整身份拒绝写入
 *  14.  僵尸进程                    -> STOPPED（S1-R）
 *  15.  状态字符分类
 *  16.  临时文件路径与清理（S2-R 修正了原假阳性路径）
 *  17.  真正的并发写入（S2-R 新增）
 *  18.  boot_id 跨重启校验（S2-R 新增）
 *  19.  单次读取快照一致性（S2-R 新增）
 *
 * 本测试不启动 Gateway，只操作临时目录中的 pidfile，
 * 并使用 fork 出来的哨兵进程模拟"被管理的进程"。
 *
 * 注：S2-R 修正了原注释与实现的偏差——旧注释声称覆盖
 * "启动后立即退出"场景，但 main() 实际没有对应测试。
 * 该场景现由 ota_supervisor_s2_test.c 的快速退出用例覆盖，
 * 此处不再声称。
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
    printf("\n== 16. 临时文件隔离与清理 ==\n");

    ota_sup_identity_t id;

    CHECK(ota_sup_capture_identity(getpid(), &id) == 0,
          "捕获身份");
    CHECK(ota_sup_pidfile_write(g_dir, &id) == 0,
          "写入 pidfile");

    /*
     * S2-R 修正：原测试用 "<install>/gateway.pid.tmp.*" 检查残留，
     * 但临时文件实际位于 "<install>/run/" 下，路径少了一层 run，
     * 导致该检查恒返回 0 —— 是**假阳性**，无论是否有残留都会通过。
     *
     * 这里改为检查正确目录，并额外确认目录确实存在，
     * 避免"目录不存在所以找不到"又被当成通过。
     */
    char run_dir[PATH_MAX + 16];
    int dn = snprintf(run_dir, sizeof(run_dir), "%s/run", g_dir);

    CHECK(dn > 0 && (size_t)dn < sizeof(run_dir), "run 路径合法");
    CHECK(access(run_dir, F_OK) == 0, "run 目录确实存在");

    char cmd[PATH_MAX + 96];
    int cn = snprintf(
        cmd, sizeof(cmd),
        "ls '%s'/gateway.pid.tmp.* 2>/dev/null | wc -l",
        run_dir);

    CHECK(cn > 0 && (size_t)cn < sizeof(cmd), "命令构造成功");

    FILE *fp = popen(cmd, "r");

    if (fp != NULL) {
        int count = -1;

        if (fscanf(fp, "%d", &count) != 1)
            count = -1;

        (void)pclose(fp);

        CHECK(count == 0,
              "run/ 目录内无残留临时文件（实际 %d 个）", count);
    }

    /*
     * 反向验证：手动放一个临时文件，确认上面的检查**能发现它**。
     *
     * 这一步很关键——只验证"没有残留"无法区分
     * "确实没有"和"检查方法本身失效"。
     */
    char planted[PATH_MAX + 64];
    int pn = snprintf(
        planted, sizeof(planted),
        "%s/gateway.pid.tmp.99999", run_dir);

    if (pn > 0 && (size_t)pn < sizeof(planted)) {
        FILE *pf = fopen(planted, "w");

        if (pf != NULL) {
            (void)fputs("planted\n", pf);
            (void)fclose(pf);

            fp = popen(cmd, "r");

            if (fp != NULL) {
                int count = -1;

                if (fscanf(fp, "%d", &count) != 1)
                    count = -1;

                (void)pclose(fp);

                CHECK(count == 1,
                      "检查方法能发现人为残留（实际 %d 个）", count);
            }

            (void)unlink(planted);
        }
    }

    /* 顺序写：后写者生效 */
    ota_sup_identity_t a = {
        .pid = getpid(), .starttime = 111, .boot_id = {0}
    };
    ota_sup_identity_t b = {
        .pid = getpid(), .starttime = 222, .boot_id = {0}
    };

    CHECK(ota_sup_pidfile_write(g_dir, &a) == 0, "写 A");
    CHECK(ota_sup_pidfile_write(g_dir, &b) == 0, "写 B");

    ota_sup_identity_t back;

    CHECK(ota_sup_pidfile_read(g_dir, &back) == 0, "读回成功");
    CHECK(back.starttime == 222,
          "后写者生效（%llu）", back.starttime);
}

/*
 * S2-R 新增：真正的并发写测试。
 *
 * 用 fork 让两个独立进程同时写同一个 pidfile，
 * 验证最终文件格式完整、可被正确解析。
 *
 * 目的不是证明"并发安全"（那需要文件锁），
 * 而是证明"不会产生半截或错乱内容"。
 */
static void test_concurrent_writers(void)
{
    printf("\n== 17. 并发写入不产生坏文件 ==\n");

    /* 找到本进程的 starttime，让两个子进程用它构造不同记录 */
    unsigned long long base = 0;

    if (ota_sup_read_starttime(getpid(), &base) != 0) {
        printf("  SKIP  无法读取 starttime\n");
        return;
    }

    /*
     * S2-R2 修正：先清掉可能已存在的 pidfile。
     *
     * 若不清，即使所有子进程写入全部失败，最后仍能读到
     * 一个"之前遗留的合法 pidfile"，测试照样通过 —— 假阳性。
     */
    (void)ota_sup_pidfile_remove(g_dir);

    ota_sup_identity_t pre;

    errno = 0;

    CHECK(ota_sup_pidfile_read(g_dir, &pre) == -1 && errno == ENOENT,
          "起始状态已清空（无遗留 pidfile）");

    const int writers = 4;
    const int rounds = 50;
    pid_t pids[8];

    if (writers > (int)(sizeof(pids) / sizeof(pids[0])))
        return;

    for (int i = 0; i < writers; i++) {
        pid_t p = fork();

        if (p == 0) {
            /*
             * 子进程：反复写，并**统计成功次数**。
             *
             * 退出码编码：成功次数（0..255 范围内）。
             * 父进程据此确认每个子进程都真的写成功过，
             * 而不是"全部失败但没人知道"。
             */
            int ok_count = 0;

            for (int r = 0; r < rounds; r++) {
                ota_sup_identity_t id = {
                    .pid = getpid(),
                    .starttime = base + (unsigned long long)(i * 1000 + r),
                    .boot_id = {0}
                };

                if (ota_sup_pidfile_write(g_dir, &id) == 0)
                    ok_count++;
            }

            _exit(ok_count > 255 ? 255 : ok_count);
        }

        pids[i] = p;
    }

    int total_ok = 0;
    int all_exited_normally = 1;

    for (int i = 0; i < writers; i++) {
        if (pids[i] <= 0)
            continue;

        int status = 0;
        pid_t r = waitpid(pids[i], &status, 0);

        if (r != pids[i] || !WIFEXITED(status)) {
            all_exited_normally = 0;
            continue;
        }

        total_ok += WEXITSTATUS(status);
    }

    CHECK(all_exited_normally,
          "所有写者进程正常退出");
    CHECK(total_ok > 0,
          "确有写入成功（累计 %d 次）", total_ok);

    /*
     * 关键断言：每个子进程至少成功写过一次。
     * 若实现有严重问题（例如全部因 EEXIST 互相删除而失败），
     * 这里会暴露，而不是被"遗留 pidfile"掩盖。
     */
    CHECK(total_ok >= writers,
          "成功次数不少于进程数（%d >= %d）",
          total_ok, writers);

    /*
     * 并发写结束后，pidfile 必须是一个**完整可解析**的记录。
     * 若临时文件名固定（旧实现），这里很可能读到半截内容。
     */
    ota_sup_identity_t final;

    int rc = ota_sup_pidfile_read(g_dir, &final);

    CHECK(rc == 0,
          "并发写后 pidfile 仍可完整解析");

    if (rc == 0) {
        CHECK(final.pid > 1, "pid 合法（%ld）", (long)final.pid);
        CHECK(final.starttime > 0,
              "starttime 合法（%llu）", final.starttime);
    }

    /* 并发的临时文件也不应残留 */
    char run_dir[PATH_MAX + 16];
    snprintf(run_dir, sizeof(run_dir), "%s/run", g_dir);

    char cmd[PATH_MAX + 96];
    snprintf(
        cmd, sizeof(cmd),
        "ls '%s'/gateway.pid.tmp.* 2>/dev/null | wc -l",
        run_dir);

    FILE *fp = popen(cmd, "r");

    if (fp != NULL) {
        int count = -1;

        if (fscanf(fp, "%d", &count) != 1)
            count = -1;

        (void)pclose(fp);

        CHECK(count == 0,
              "并发后无临时文件残留（实际 %d 个）", count);
    }
}

/*
 * S2-R 新增：boot_id 跨重启身份校验。
 */
static void test_boot_id(void)
{
    printf("\n== 18. boot_id 跨重启校验 ==\n");

    char boot_id[OTA_SUP_BOOT_ID_MAX];

    int rc = ota_sup_read_boot_id(boot_id, sizeof(boot_id));

    CHECK(rc == 0, "能读取 boot_id");

    if (rc != 0) {
        printf("  NOTE  boot_id 不可用（%s），跨重启校验将退化\n",
               strerror(errno));
        return;
    }

    printf("  boot_id = %s\n", boot_id);
    CHECK(strlen(boot_id) == 36,
          "boot_id 长度为 36（实际 %zu）", strlen(boot_id));

    /* 同一启动周期内，捕获的身份应带上 boot_id */
    ota_sup_identity_t id;

    CHECK(ota_sup_capture_identity(getpid(), &id) == 0,
          "捕获身份");
    CHECK(strcmp(id.boot_id, boot_id) == 0,
          "身份中的 boot_id 与当前一致");

    CHECK(ota_sup_pidfile_write(g_dir, &id) == 0,
          "写入带 boot_id 的 pidfile");

    ota_sup_identity_t back;

    CHECK(ota_sup_pidfile_read(g_dir, &back) == 0,
          "读回成功");
    CHECK(strcmp(back.boot_id, boot_id) == 0,
          "boot_id 往返一致");

    ota_sup_state_t st = ota_sup_verify_identity(&back);

    CHECK(st == OTA_SUP_RUNNING,
          "同启动周期 -> RUNNING（实际 %s）",
          ota_sup_state_name(st));

    /*
     * 模拟"来自上一次启动"的记录：
     * 除 boot_id 外一切相同，必须判为 STOPPED。
     */
    ota_sup_identity_t stale = back;

    for (size_t i = 0; i < sizeof(stale.boot_id); i++)
        stale.boot_id[i] = '\0';

    snprintf(stale.boot_id, sizeof(stale.boot_id),
             "00000000-1111-2222-3333-444444444444");

    st = ota_sup_verify_identity(&stale);

    CHECK(st == OTA_SUP_STOPPED,
          "boot_id 不同（模拟跨重启）-> STOPPED（实际 %s）",
          ota_sup_state_name(st));

    /* 兼容性：无 boot_id 的旧格式必须仍被接受 */
    ota_sup_identity_t legacy = back;

    memset(legacy.boot_id, 0, sizeof(legacy.boot_id));

    st = ota_sup_verify_identity(&legacy);

    CHECK(st == OTA_SUP_RUNNING,
          "无 boot_id 的旧记录仍按 (pid,starttime) 判定 -> RUNNING");

    /* 旧格式 pidfile 文本解析 */
    write_raw_pidfile("4242 99999\n");

    ota_sup_identity_t old_fmt;

    CHECK(ota_sup_pidfile_read(g_dir, &old_fmt) == 0,
          "两段式旧格式仍可解析");
    CHECK(old_fmt.pid == 4242, "旧格式 pid 正确");
    CHECK(old_fmt.starttime == 99999, "旧格式 starttime 正确");
    CHECK(old_fmt.boot_id[0] == '\0', "旧格式 boot_id 为空");

    /* 三段式新格式 */
    write_raw_pidfile(
        "4242 99999 1f2e3d4c-5b6a-7890-abcd-ef0123456789\n");

    ota_sup_identity_t new_fmt;

    CHECK(ota_sup_pidfile_read(g_dir, &new_fmt) == 0,
          "三段式新格式可解析");
    CHECK(strcmp(new_fmt.boot_id,
                 "1f2e3d4c-5b6a-7890-abcd-ef0123456789") == 0,
          "新格式 boot_id 正确");
}

/*
 * S2-R 新增：一次读取的一致性。
 *
 * 验证 probe_process 返回的两个字段来自同一份 stat 内容：
 * 对同一进程，probe 得到的 starttime 必须与单独 read_starttime 一致，
 * state 必须与单独 read_state 一致。
 */
static void test_probe_consistency(void)
{
    printf("\n== 19. 单次读取快照一致性 ==\n");

    char st_probe = '\0';
    unsigned long long st_time = 0;

    int rc = ota_sup_probe_process(
        getpid(), &st_probe, &st_time);

    CHECK(rc == 0, "probe_process 成功");

    char st_single = '\0';
    unsigned long long time_single = 0;

    CHECK(ota_sup_read_state(getpid(), &st_single) == 0,
          "read_state 成功");
    CHECK(ota_sup_read_starttime(getpid(), &time_single) == 0,
          "read_starttime 成功");

    CHECK(st_probe == st_single,
          "state 一致（'%c' vs '%c'）", st_probe, st_single);
    CHECK(st_time == time_single,
          "starttime 一致（%llu vs %llu）",
          st_time, time_single);

    /* 只取 state 时不应要求 starttime */
    char only_state = '\0';

    CHECK(ota_sup_read_state(getpid(), &only_state) == 0,
          "仅取 state 也可成功");

    /* 只取 starttime 时不应要求 state */
    unsigned long long only_time = 0;

    CHECK(ota_sup_read_starttime(getpid(), &only_time) == 0,
          "仅取 starttime 也可成功");

    /* 两个都不要求应被拒绝 */
    errno = 0;

    CHECK(ota_sup_probe_process(getpid(), NULL, NULL) == -1 &&
          errno == EINVAL,
          "两个输出都为 NULL 被拒绝");

    /* 不存在的 pid */
    errno = 0;

    CHECK(ota_sup_probe_process(999999, &st_probe, &st_time) == -1 &&
          errno == ENOENT,
          "不存在的 pid -> ENOENT");
}

/*
 * S2-R2 新增：临时文件唯一性（同进程多次调用不撞名）。
 *
 * 这是"问题二"的直接验证：原实现用 pid 作后缀，
 * 同一进程的多个线程会得到相同临时名。
 * 现在加了原子序号，连续调用必须产生不同临时文件。
 */
static void test_temp_uniqueness(void)
{
    printf("\n== 20. 临时文件名唯一性 ==\n");

    /*
     * 连续写入多个身份，确认每次都成功——
     * 若临时名重复，第二次会因 O_EXCL 失败。
     */
    int ok = 0;

    for (int i = 0; i < 20; i++) {
        ota_sup_identity_t id = {
            .pid = getpid(),
            .starttime = (unsigned long long)(7000 + i),
            .boot_id = {0}
        };

        if (ota_sup_pidfile_write(g_dir, &id) == 0)
            ok++;
    }

    CHECK(ok == 20,
          "20 次连续写入全部成功（实际 %d 次）", ok);

    ota_sup_identity_t back;

    CHECK(ota_sup_pidfile_read(g_dir, &back) == 0, "最终可读");
    CHECK(back.starttime == 7019,
          "最后一次生效（%llu）", back.starttime);
}

/*
 * S2-R2 新增：孤儿临时文件清理。
 *
 * 验证 ota_sup_cleanup_stale_temp()：
 *   - 不删除"新鲜"的临时文件（可能是活跃写者）；
 *   - 删除"陈旧"的临时文件（崩溃遗留）。
 */
static void test_stale_temp_cleanup(void)
{
    printf("\n== 21. 孤儿临时文件清理 ==\n");

    char run_dir[PATH_MAX + 16];
    snprintf(run_dir, sizeof(run_dir), "%s/run", g_dir);
    (void)mkdir(run_dir, 0700);

    char fresh[PATH_MAX + 64];
    char stale[PATH_MAX + 64];

    snprintf(fresh, sizeof(fresh),
             "%s/gateway.pid.tmp.111.1", run_dir);
    snprintf(stale, sizeof(stale),
             "%s/gateway.pid.tmp.222.2", run_dir);

    FILE *f = fopen(fresh, "w");
    if (f != NULL) {
        (void)fputs("fresh\n", f);
        (void)fclose(f);
    }

    f = fopen(stale, "w");
    if (f != NULL) {
        (void)fputs("stale\n", f);
        (void)fclose(f);
    }

    /* 把 stale 的 mtime 改成 2 小时前 */
    struct timespec times[2];
    times[0].tv_sec = time(NULL) - 7200;
    times[0].tv_nsec = 0;
    times[1] = times[0];

    (void)utimensat(AT_FDCWD, stale, times, 0);

    int removed = ota_sup_cleanup_stale_temp(g_dir, 60);

    CHECK(removed == 1,
          "只删除 1 个陈旧临时文件（实际 %d）", removed);
    CHECK(access(stale, F_OK) != 0,
          "陈旧临时文件已删除");
    CHECK(access(fresh, F_OK) == 0,
          "新鲜临时文件被保留（不误删活跃写者）");

    (void)unlink(fresh);
}

/*
 * S2-R2 新增：旧格式识别。
 */
static void test_legacy_detection(void)
{
    printf("\n== 22. 旧格式 pidfile 识别 ==\n");

    int is_legacy = -1;

    /* 旧格式（无 boot_id） */
    write_raw_pidfile("5555 12345\n");

    CHECK(ota_sup_identity_is_legacy(g_dir, &is_legacy) == 0,
          "旧格式可识别");
    CHECK(is_legacy == 1,
          "识别为旧格式（实际 %d）", is_legacy);

    /* 新格式（含 boot_id） */
    write_raw_pidfile(
        "5555 12345 0a023e15-9190-44fa-becc-c3dfb34712ba\n");

    CHECK(ota_sup_identity_is_legacy(g_dir, &is_legacy) == 0,
          "新格式可识别");
    CHECK(is_legacy == 0,
          "识别为新格式（实际 %d）", is_legacy);

    /* 无 pidfile */
    (void)ota_sup_pidfile_remove(g_dir);

    errno = 0;

    CHECK(ota_sup_identity_is_legacy(g_dir, &is_legacy) == -1 &&
          errno == ENOENT,
          "无记录时返回 -1/ENOENT");
}

int main(void)
{
    printf("ota_supervisor 单元测试（D25-2 S1 + S1-R + S2-R）\n");

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
    test_concurrent_writers();
    test_boot_id();
    test_probe_consistency();
    test_temp_uniqueness();
    test_stale_temp_cleanup();
    test_legacy_detection();

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
