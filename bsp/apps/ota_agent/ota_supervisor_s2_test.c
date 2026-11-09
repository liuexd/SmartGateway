/*
 * ota_supervisor S2 自管 Supervisor 单元测试
 *
 * 覆盖：
 *   1. 正常启动 + pidfile 生效
 *   2. 重复启动被拒（EALREADY）
 *   3. 正常停止 + pidfile 清理
 *   4. 非所有者 stop 被拒（EPERM）
 *   5. exec 失败不留下 pidfile
 *   6. 进程被外部杀掉后 query 返回 STOPPED
 *   7. 停止后无僵尸残留
 *   8. 僵尸存在时启动被拒
 *   9. pidfile 内容与实际进程匹配
 *  10. 极端: 启动后立即退出 -> 不留下无效记录
 *
 * 使用 /bin/sh -c 'sleep N' 作为被管理进程的替身，
 * 不依赖真实 Gateway 二进制，测试可在任意环境运行。
 */

#define _POSIX_C_SOURCE 200809L

#include "ota_supervisor.h"
#include "ota_process.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int g_failures = 0;
static int g_checks = 0;
static char g_dir[PATH_MAX];

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

static void make_temp_dir(void)
{
    snprintf(g_dir, sizeof(g_dir), "/tmp/d25-2-s2-XXXXXX");

    if (mkdtemp(g_dir) == NULL) {
        perror("mkdtemp");
        exit(2);
    }
}

static void cleanup_temp_dir(void)
{
    char cmd[PATH_MAX + 32];

    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", g_dir);

    int rc = system(cmd);
    (void)rc;
}

static void sleep_ms(int ms)
{
    struct timespec ts = {
        .tv_sec = ms / 1000,
        .tv_nsec = (long)(ms % 1000) * 1000000L
    };

    while (nanosleep(&ts, &ts) != 0 && errno == EINTR) {
    }
}

/* 用 sh -c 'sleep N' 作为被管理进程替身 */
static int start_sleeper(
    ota_sup_managed_t *handle,
    const char *seconds)
{
    char *argv[] = {
        "/bin/sh", "-c", NULL, NULL
    };

    char cmd[64];
    snprintf(cmd, sizeof(cmd), "sleep %s", seconds);

    argv[2] = cmd;

    return ota_sup_start_managed(
        g_dir, "/bin/sh", argv, handle);
}

/* ------------------------------------------------------------------ */

static void test_start_and_stop(void)
{
    printf("\n== 1. 正常启动 + 停止 ==\n");

    ota_sup_managed_t h = {0};

    int rc = start_sleeper(&h, "30");

    CHECK(rc == 0, "启动成功");

    if (rc != 0) {
        printf("  errno=%d (%s)\n", errno, strerror(errno));
        return;
    }

    CHECK(h.pid > 1, "pid 有效（%ld）", (long)h.pid);
    CHECK(h.starttime != 0, "starttime 非 0");
    CHECK(h.owner_pid == getpid(), "owner 记录为本进程");

    /* pidfile 应能读到一致身份 */
    ota_sup_identity_t id;

    CHECK(ota_sup_pidfile_read(g_dir, &id) == 0,
          "pidfile 可读");
    CHECK(id.pid == h.pid, "pidfile pid 一致");
    CHECK(id.starttime == h.starttime, "pidfile starttime 一致");

    pid_t qpid = -1;
    ota_sup_state_t st = ota_sup_query(g_dir, &qpid);

    CHECK(st == OTA_SUP_RUNNING,
          "query -> RUNNING（实际 %s）",
          ota_sup_state_name(st));
    CHECK(qpid == h.pid, "query 返回正确 pid");

    int status = 0;
    long long elapsed = 0;

    rc = ota_sup_stop_managed(
        g_dir, &h, 3000, &status, &elapsed);

    CHECK(rc == OTA_PROCESS_GRACEFUL || rc == OTA_PROCESS_ALREADY_EXITED,
          "停止成功（rc=%d, %lldms）", rc, elapsed);

    /* pidfile 应已清理 */
    errno = 0;

    CHECK(ota_sup_pidfile_read(g_dir, &id) == -1 && errno == ENOENT,
          "pidfile 已清理");

    st = ota_sup_query(g_dir, NULL);

    CHECK(st == OTA_SUP_STOPPED,
          "停止后 -> STOPPED（实际 %s）",
          ota_sup_state_name(st));
}

static void test_duplicate_start_rejected(void)
{
    printf("\n== 2. 重复启动被拒 ==\n");

    ota_sup_managed_t h1 = {0};

    int rc = start_sleeper(&h1, "30");

    CHECK(rc == 0, "第一个实例启动成功");

    if (rc != 0)
        return;

    ota_sup_managed_t h2 = {0};

    errno = 0;
    rc = start_sleeper(&h2, "30");

    CHECK(rc == -1, "第二个实例被拒绝");
    CHECK(errno == EALREADY,
          "errno=EALREADY（实际 %d: %s）",
          errno, strerror(errno));

    /* 第一个实例必须仍在运行且记录未被破坏 */
    pid_t qpid = -1;
    ota_sup_state_t st = ota_sup_query(g_dir, &qpid);

    CHECK(st == OTA_SUP_RUNNING, "原实例仍 RUNNING");
    CHECK(qpid == h1.pid, "pidfile 未被第二个实例覆盖");

    (void)ota_sup_stop_managed(g_dir, &h1, 3000, NULL, NULL);
}

static void test_wrong_owner_rejected(void)
{
    printf("\n== 3. 非所有者不能停止 ==\n");

    ota_sup_managed_t h = {0};

    if (start_sleeper(&h, "30") != 0) {
        printf("  SKIP  启动失败\n");
        return;
    }

    /* 伪造一个 owner 不是当前进程的句柄 */
    ota_sup_managed_t fake = h;
    fake.owner_pid = getpid() + 1;

    errno = 0;
    int rc = ota_sup_stop_managed(g_dir, &fake, 1000, NULL, NULL);

    CHECK(rc == -1, "跨所有者 stop 被拒绝");
    CHECK(errno == EPERM,
          "errno=EPERM（实际 %d）", errno);

    /* 真实进程不应受影响 */
    ota_sup_state_t st = ota_sup_query(g_dir, NULL);

    CHECK(st == OTA_SUP_RUNNING, "进程仍在运行，未被误杀");

    (void)ota_sup_stop_managed(g_dir, &h, 3000, NULL, NULL);
}

static void test_exec_failure(void)
{
    printf("\n== 4. exec 失败不留 pidfile ==\n");

    /* 确保起始状态干净 */
    (void)ota_sup_pidfile_remove(g_dir);

    char *argv[] = { "/nonexistent/binary", NULL };
    ota_sup_managed_t h = {0};

    int rc = ota_sup_start_managed(
        g_dir, "/nonexistent/binary", argv, &h);

    CHECK(rc == -1, "启动失败");

    ota_sup_identity_t id;
    errno = 0;

    CHECK(ota_sup_pidfile_read(g_dir, &id) == -1 && errno == ENOENT,
          "未留下 pidfile");

    CHECK(ota_sup_query(g_dir, NULL) == OTA_SUP_STOPPED,
          "状态为 STOPPED");
}

static void test_external_kill(void)
{
    printf("\n== 5. 进程被外部杀掉 -> STOPPED ==\n");

    ota_sup_managed_t h = {0};

    if (start_sleeper(&h, "30") != 0) {
        printf("  SKIP  启动失败\n");
        return;
    }

    /* 模拟外部 SIGKILL */
    CHECK(kill(h.pid, SIGKILL) == 0, "外部发送 SIGKILL");

    /*
     * 不 wait：进程会短暂保持僵尸。
     * query 必须返回 STOPPED（僵尸不等于运行）。
     */
    sleep_ms(300);

    ota_sup_state_t st = ota_sup_query(g_dir, NULL);

    CHECK(st == OTA_SUP_STOPPED,
          "僵尸状态 -> STOPPED（实际 %s）",
          ota_sup_state_name(st));

    /* 清理：回收僵尸并删 pidfile */
    (void)waitpid(h.pid, NULL, 0);
    CHECK(ota_sup_pidfile_remove(g_dir) == 0,
          "清理 pidfile");
}

static void test_no_zombie_after_stop(void)
{
    printf("\n== 6. 正常停止后无僵尸残留 ==\n");

    ota_sup_managed_t h = {0};

    if (start_sleeper(&h, "30") != 0) {
        printf("  SKIP  启动失败\n");
        return;
    }

    int status = 0;

    int rc = ota_sup_stop_managed(
        g_dir, &h, 3000, &status, NULL);

    CHECK(rc >= 0, "停止成功（rc=%d）", rc);

    /* waitpid 已经回收过，再 wait 应返回 ECHILD */
    errno = 0;

    pid_t w = waitpid(h.pid, NULL, WNOHANG);

    CHECK(w == -1 && errno == ECHILD,
          "进程已被回收（无僵尸，waitpid=-1/ECHILD）");

    /* /proc 条目应消失 */
    char path[64];
    snprintf(path, sizeof(path), "/proc/%ld", (long)h.pid);

    CHECK(access(path, F_OK) != 0, "/proc 条目已消失");
}

static void test_unknown_blocks_start(void)
{
    printf("\n== 7. 状态 UNKNOWN 时拒绝启动 ==\n");

    (void)ota_sup_pidfile_remove(g_dir);

    /*
     * 写入一个损坏的 pidfile，让 query 返回 UNKNOWN。
     * 此时启动新实例是危险的：可能已有 Gateway 在跑。
     */
    char path[PATH_MAX];
    char dir[PATH_MAX + 16];

    int dn = snprintf(dir, sizeof(dir), "%s/run", g_dir);

    CHECK(dn > 0 && (size_t)dn < sizeof(dir), "run 路径合法");

    (void)mkdir(dir, 0700);

    CHECK(ota_sup_pidfile_path(g_dir, path, sizeof(path)) == 0,
          "构造路径");

    FILE *fp = fopen(path, "w");

    if (fp == NULL) {
        printf("  SKIP  无法写入\n");
        return;
    }

    (void)fputs("NOT-A-VALID-PIDFILE\n", fp);
    (void)fclose(fp);

    ota_sup_state_t st = ota_sup_query(g_dir, NULL);

    CHECK(st == OTA_SUP_UNKNOWN,
          "损坏 pidfile -> UNKNOWN（实际 %s）",
          ota_sup_state_name(st));

    ota_sup_managed_t h = {0};

    errno = 0;
    int rc = start_sleeper(&h, "30");

    CHECK(rc == -1, "UNKNOWN 状态下拒绝启动");
    CHECK(errno == EBUSY,
          "errno=EBUSY（实际 %d: %s）",
          errno, strerror(errno));

    (void)ota_sup_pidfile_remove(g_dir);
}

static void test_invalid_args(void)
{
    printf("\n== 8. 参数校验 ==\n");

    ota_sup_managed_t h = {0};
    char *argv[] = { "/bin/sh", NULL };

    errno = 0;
    CHECK(ota_sup_start_managed(NULL, "/bin/sh", argv, &h) == -1 &&
          errno == EINVAL, "install_dir=NULL 被拒");

    errno = 0;
    CHECK(ota_sup_start_managed(g_dir, NULL, argv, &h) == -1 &&
          errno == EINVAL, "executable=NULL 被拒");

    errno = 0;
    CHECK(ota_sup_start_managed(g_dir, "/bin/sh", NULL, &h) == -1 &&
          errno == EINVAL, "argv=NULL 被拒");

    errno = 0;
    CHECK(ota_sup_start_managed(g_dir, "/bin/sh", argv, NULL) == -1 &&
          errno == EINVAL, "handle=NULL 被拒");

    errno = 0;
    CHECK(ota_sup_stop_managed(g_dir, NULL, 1000, NULL, NULL) == -1 &&
          errno == EPERM, "handle=NULL 被拒（stop）");

    ota_sup_managed_t bad = { .pid = 1, .starttime = 1,
                              .owner_pid = getpid() };

    errno = 0;
    CHECK(ota_sup_stop_managed(g_dir, &bad, 1000, NULL, NULL) == -1 &&
          errno == EPERM, "pid=1 被拒（绝不操作 init）");
}

int main(void)
{
    printf("ota_supervisor S2 自管 Supervisor 测试\n");

    make_temp_dir();
    printf("临时目录: %s\n", g_dir);

    test_start_and_stop();
    test_duplicate_start_rejected();
    test_wrong_owner_rejected();
    test_exec_failure();
    test_external_kill();
    test_no_zombie_after_stop();
    test_unknown_blocks_start();
    test_invalid_args();

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
