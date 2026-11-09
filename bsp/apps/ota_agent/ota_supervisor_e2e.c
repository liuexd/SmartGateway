/*
 * D25-2 S2 端到端验证：用真实 ARM Gateway 走 Supervisor 全流程
 *
 * 不是单元测试，而是集成验证：
 *   启动真实 Gateway -> query 确认 RUNNING -> 重复启动被拒
 *   -> 优雅停止 -> 确认 STOPPED -> pidfile 已清理 -> 无残留
 *
 * 用法：./e2e <install_dir> <gateway_path>
 */

#define _POSIX_C_SOURCE 200809L

#include "ota_supervisor.h"
#include "ota_process.h"

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static int failures;

static void check(int cond, const char *label)
{
    printf("  %s  %s\n", cond ? "PASS" : "FAIL", label);

    if (!cond)
        failures++;
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

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr,
                "用法: %s <install_dir> <gateway_path>\n", argv[0]);
        return 2;
    }

    const char *install = argv[1];
    const char *gw = argv[2];

    printf("D25-2 S2 端到端验证\n");
    printf("install = %s\n", install);
    printf("gateway = %s\n\n", gw);

    /* ---- 0. 初始状态应为 STOPPED ---- */
    printf("== 0. 初始状态 ==\n");

    ota_sup_state_t st = ota_sup_query(install, NULL);

    check(st == OTA_SUP_STOPPED,
          "初始 STOPPED");

    /* ---- 1. 启动真实 Gateway ---- */
    printf("\n== 1. 启动真实 Gateway ==\n");

    char *gw_argv[] = {
        (char *)gw,
        "--no-serial",
        "--server", "127.0.0.1",
        "--port", "19000",
        NULL
    };

    ota_sup_managed_t h = {0};

    int rc = ota_sup_start_managed(
        install, gw, gw_argv, &h);

    check(rc == 0, "启动成功");

    if (rc != 0) {
        printf("  errno=%d (%s)\n", errno, strerror(errno));
        return 1;
    }

    printf("  pid=%ld starttime=%llu\n",
           (long)h.pid, h.starttime);

    /* ---- 2. query 应为 RUNNING ---- */
    printf("\n== 2. 身份校验 ==\n");

    pid_t qpid = -1;

    st = ota_sup_query(install, &qpid);

    check(st == OTA_SUP_RUNNING, "query -> RUNNING");
    check(qpid == h.pid, "pid 一致");

    /* pidfile 内容应与句柄一致 */
    ota_sup_identity_t id;

    check(ota_sup_pidfile_read(install, &id) == 0,
          "pidfile 可读");
    check(id.pid == h.pid && id.starttime == h.starttime,
          "pidfile 身份与句柄一致");

    /* 让 Gateway 完成线程创建 */
    sleep_ms(2500);

    /* ---- 3. 重复启动被拒 ---- */
    printf("\n== 3. 重复启动保护 ==\n");

    ota_sup_managed_t h2 = {0};

    errno = 0;
    rc = ota_sup_start_managed(install, gw, gw_argv, &h2);

    check(rc == -1 && errno == EALREADY,
          "第二个实例被拒（EALREADY）");

    /* 原实例必须仍在 */
    st = ota_sup_query(install, &qpid);

    check(st == OTA_SUP_RUNNING && qpid == h.pid,
          "原实例未受影响");

    /* ---- 4. 优雅停止 ---- */
    printf("\n== 4. 优雅停止 ==\n");

    int status = 0;
    long long elapsed = 0;

    rc = ota_sup_stop_managed(
        install, &h, 5000, &status, &elapsed);

    printf("  stop rc=%d elapsed=%lldms exit=%d\n",
           rc, elapsed,
           WIFEXITED(status) ? WEXITSTATUS(status) : -1);

    check(rc == OTA_PROCESS_GRACEFUL,
          "SIGTERM 优雅退出（未用 SIGKILL）");
    check(WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "退出码为 0");

    /* ---- 5. 清理确认 ---- */
    printf("\n== 5. 停止后状态 ==\n");

    st = ota_sup_query(install, NULL);

    check(st == OTA_SUP_STOPPED, "query -> STOPPED");

    errno = 0;
    check(ota_sup_pidfile_read(install, &id) == -1 &&
          errno == ENOENT,
          "pidfile 已清理");

    /* 无僵尸 */
    errno = 0;
    pid_t w = waitpid(h.pid, NULL, WNOHANG);

    check(w == -1 && errno == ECHILD,
          "无僵尸残留（waitpid=ECHILD）");

    char procpath[64];
    snprintf(procpath, sizeof(procpath), "/proc/%ld", (long)h.pid);

    check(access(procpath, F_OK) != 0, "/proc 条目已消失");

    printf("\n============================\n");

    if (failures != 0) {
        printf("失败项: %d\n结果: FAIL\n", failures);
        return 1;
    }

    printf("结果: PASS\n");
    return 0;
}
