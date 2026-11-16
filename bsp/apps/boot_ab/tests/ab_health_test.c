#define _POSIX_C_SOURCE 200809L
#include "ab_health.h"
#include "service_ipc.h"

#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(cond) do {     if (!(cond)) {         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         g_fail++;     } else {         g_pass++;     } } while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))

static void write_text(const char *path, const char *txt)
{
    FILE *fp = fopen(path, "w");
    ASSERT_TRUE(fp != NULL);
    if (fp) {
        fputs(txt, fp);
        fclose(fp);
    }
}

typedef struct {
    int listen_fd;
    service_ipc_result_t reply_result;
    pid_t reply_pid;
    unsigned long long reply_start;
    int max_requests;
} mock_sup_ctx_t;

static void serve_n_requests(mock_sup_ctx_t *ctx)
{
    for (int i = 0; i < ctx->max_requests; i++) {
        service_ipc_request_t req = SERVICE_IPC_UNKNOWN;
        int cfd = service_ipc_accept_request(ctx->listen_fd, &req);
        if (cfd < 0)
            break;
        service_ipc_reply_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.result = ctx->reply_result;
        rep.pid = ctx->reply_pid;
        rep.starttime = ctx->reply_start;
        snprintf(rep.detail, sizeof(rep.detail), "%s",
                 ctx->reply_result == SERVICE_IPC_OK ? "running" : "down");
        service_ipc_send_reply_and_close(cfd, &rep);
    }
}

int main(void)
{
    char tmpdir[] = "/tmp/sgw_health_test_XXXXXX";
    char cmdline_path[256];
    char mounts_path[256];
    char run_dir[256];
    char sysroot_dir[256];
    char sbin_dir[320];
    char etc_dir[320];
    char init_path[384];
    char inittab_path[384];
    char err[256] = {0};
    ab_health_cfg_t cfg;
    ab_env_t env;
    ab_slot_t slot = AB_SLOT_NONE;
    unsigned long long svc_pid = 0;

    ASSERT_TRUE(mkdtemp(tmpdir) != NULL);
    snprintf(cmdline_path, sizeof(cmdline_path), "%s/cmdline", tmpdir);
    snprintf(mounts_path, sizeof(mounts_path), "%s/mounts", tmpdir);
    snprintf(run_dir, sizeof(run_dir), "%s/run", tmpdir);
    snprintf(sysroot_dir, sizeof(sysroot_dir), "%s/sysroot", tmpdir);
    snprintf(sbin_dir, sizeof(sbin_dir), "%s/sbin", sysroot_dir);
    snprintf(etc_dir, sizeof(etc_dir), "%s/etc", sysroot_dir);
    snprintf(init_path, sizeof(init_path), "%s/init", sbin_dir);
    snprintf(inittab_path, sizeof(inittab_path), "%s/inittab", etc_dir);

    mkdir(run_dir, 0755);
    mkdir(sysroot_dir, 0755);
    mkdir(sbin_dir, 0755);
    mkdir(etc_dir, 0755);

    ab_health_cfg_init(&cfg);
    cfg.proc_cmdline_path = cmdline_path;
    cfg.proc_mounts_path = mounts_path;
    cfg.supervisor_run_dir = run_dir;
    cfg.sysroot_prefix = sysroot_dir;
    cfg.supervisor_timeout_ms = 1000;

    /* 1. Unknown cmdline root fails closed */
    write_text(cmdline_path, "console=ttymxc0,115200 root=/dev/mmcblk0p2 rw\n");
    write_text(mounts_path, "/dev/root / ext4 rw 0 0\n");
    ASSERT_TRUE(ab_detect_running_slot(&cfg, &slot, err, sizeof(err)) != 0);

    /* 2. Split-brain cmdline vs mounts fails closed */
    write_text(cmdline_path, "console=ttymxc0,115200 root=/dev/mmcblk1p1 rootwait rw\n");
    write_text(mounts_path, "/dev/mmcblk1p2 / ext4 rw 0 0\n");
    ASSERT_TRUE(ab_detect_running_slot(&cfg, &slot, err, sizeof(err)) != 0);

    /* 3. Consistent p1 running detection succeeds */
    write_text(mounts_path, "/dev/root / ext4 rw 0 0\n");
    ASSERT_EQ(ab_detect_running_slot(&cfg, &slot, err, sizeof(err)), 0);
    ASSERT_EQ(slot, AB_SLOT_1);

    /* Stage env in trial p1 */
    ab_env_init_default(&env);
    ASSERT_EQ(ab_stage_trial(&env, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)), 0);

    /* 4. Missing /sbin/init & /etc/inittab rejected */
    ASSERT_TRUE(ab_verify_system_health(&cfg, &env, &slot, &svc_pid, err, sizeof(err)) != 0);

    write_text(init_path, "init\n");
    write_text(inittab_path, "::sysinit:/etc/init.d/rcS\n");

    /* 5. Read-only rootfs rejected */
    write_text(mounts_path, "/dev/root / ext4 ro 0 0\n");
    ASSERT_TRUE(ab_verify_system_health(&cfg, &env, &slot, &svc_pid, err, sizeof(err)) != 0);
    write_text(mounts_path, "/dev/root / ext4 rw 0 0\n");

    /* 6. Missing supervisor socket rejected (fail closed) */
    ASSERT_TRUE(ab_verify_system_health(&cfg, &env, &slot, &svc_pid, err, sizeof(err)) != 0);

    /* 7. Live supervisor socket + live child service */
    pid_t gw_pid = fork();
    ASSERT_TRUE(gw_pid >= 0);
    if (gw_pid == 0) {
        pause();
        _exit(0);
    }

    char sock_path[256] = {0};
    int lfd = service_ipc_listen(run_dir, sock_path, sizeof(sock_path));
    ASSERT_TRUE(lfd >= 0);

    pid_t srv_pid = fork();
    ASSERT_TRUE(srv_pid >= 0);
    if (srv_pid == 0) {
        mock_sup_ctx_t mctx;
        mctx.listen_fd = lfd;
        /* Req 1: NOT_RUNNING, Req 2: OK with gw_pid, Req 3: OK after gw killed */
        mctx.max_requests = 1;
        mctx.reply_result = SERVICE_IPC_NOT_RUNNING;
        mctx.reply_pid = 0;
        mctx.reply_start = 0;
        serve_n_requests(&mctx);

        mctx.reply_result = SERVICE_IPC_OK;
        mctx.reply_pid = gw_pid;
        mctx.reply_start = 12345ULL;
        serve_n_requests(&mctx);
        serve_n_requests(&mctx);
        close(lfd);
        _exit(0);
    }
    close(lfd);

    /* 7a. Supervisor reports NOT_RUNNING -> health check rejected */
    ASSERT_TRUE(ab_verify_system_health(&cfg, &env, &slot, &svc_pid, err, sizeof(err)) != 0);

    /* 7b. Supervisor reports OK with live gw_pid -> health check PASS */
    ASSERT_EQ(ab_verify_system_health(&cfg, &env, &slot, &svc_pid, err, sizeof(err)), 0);
    ASSERT_EQ(slot, AB_SLOT_1);
    ASSERT_EQ(svc_pid, (unsigned long long)gw_pid);

    /* 7c. Gateway process dead even if supervisor stale -> rejected by kill(pid,0) */
    kill(gw_pid, SIGKILL);
    waitpid(gw_pid, NULL, 0);
    ASSERT_TRUE(ab_verify_system_health(&cfg, &env, &slot, &svc_pid, err, sizeof(err)) != 0);

    waitpid(srv_pid, NULL, 0);

    unlink(sock_path);
    unlink(init_path);
    unlink(inittab_path);
    rmdir(sbin_dir);
    rmdir(etc_dir);
    rmdir(sysroot_dir);
    rmdir(run_dir);
    unlink(cmdline_path);
    unlink(mounts_path);
    rmdir(tmpdir);

    printf("ab_health_test: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
