
#define _POSIX_C_SOURCE 200809L

#include "ota_verify.h"
#include "ota_supervisor.h"
#include "ota_state_io.h"
#include "ota_oplog.h"
#include "ota_recover.h"
#include "ota_lock.h"
#include "ota_release.h"
#include "ota_upgrade.h"
#include "ota_upgrade_run.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* 将版本号映射到 releases 目录 */
static int build_target(
    const char *version,
    char *output,
    size_t capacity)
{
    const char *prefix =
        version[0] == 'v' ? "releases/" : "releases/v";

    int n = snprintf(
        output, capacity, "%s%s", prefix, version);

    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}

/* 只读查询 OTA 状态及当前版本 */
static int show_status(const char *install_dir)
{
    char state_dir[PATH_MAX];
    char current[128];
    char confirmed_target[128];
    char candidate_target[128];

    ota_state_record_t record = {0};
    int dirfd = -1;
    int result = 1;

    int n = snprintf(
        state_dir, sizeof(state_dir),
        "%s/state", install_dir);

    if (n < 0 || (size_t)n >= sizeof(state_dir)) {
        fprintf(stderr, "FAIL: state path too long\n");
        return 1;
    }

    if (ota_state_load(state_dir, &record) != 0) {
        fprintf(stderr, "FAIL: cannot load OTA state\n");
        return 1;
    }

    if (build_target(
            record.confirmed_version,
            confirmed_target,
            sizeof(confirmed_target)) != 0 ||
        build_target(
            record.candidate_version,
            candidate_target,
            sizeof(candidate_target)) != 0) {
        fprintf(stderr, "FAIL: invalid release target\n");
        return 1;
    }

    dirfd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY |
        O_CLOEXEC | O_NOFOLLOW);

    if (dirfd < 0) {
        perror("open install directory");
        return 1;
    }

    ssize_t len = readlinkat(
        dirfd, "current",
        current, sizeof(current) - 1);

    if (len < 0 || (size_t)len >= sizeof(current) - 1) {
        fprintf(stderr, "FAIL: cannot read current\n");
        goto cleanup;
    }

    current[len] = '\0';

    printf("State     : %s\n",
           ota_state_name(record.state));

    printf("Confirmed : %s\n",
           record.confirmed_version);

    printf("Candidate : %s\n",
           record.candidate_version);

    printf("Current   : %s\n", current);

    if (strcmp(current, confirmed_target) == 0) {
        puts("Release   : CONFIRMED_TARGET");
    } else if (strcmp(current, candidate_target) == 0) {
        puts("Release   : CANDIDATE_TARGET");
        puts("NOTICE: candidate is not necessarily healthy");
    } else {
        fprintf(stderr,
                "FAIL: current points to unknown release\n");
        goto cleanup;
    }

    result = 0;

cleanup:
    close(dirfd);
    return result;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s verify PACKAGE_DIR\n"
            "  %s status INSTALL_DIR\n"
            "  %s recover INSTALL_DIR\n"
            "  %s stage PACKAGE_DIR INSTALL_DIR\n"
            "  %s ack-operation INSTALL_DIR\n"
            "  %s PACKAGE_DIR INSTALL_DIR\n",
            program, program, program, program, program, program);
}

/*
 * ack-operation - 人工确认上次未完成的升级已被处理（S4/F2）
 *
 * 为什么需要这条命令
 * ------------------
 * ota_upgrade_run_test() 检测到 last_operation.json 表示"上次没有
 * 正常收尾"时，会拒绝继续升级。这是**有意**的：残留可能来自
 * "旧版已停、新版未起"这种业务中断状态，OTA 无权替运维判定它无害。
 *
 * 但拒绝之后必须有一条**显式**路径让人解除阻塞，否则遗留文件会
 * 让安装目录永久无法升级，最终逼着人手写 rm——那比不做检查更糟。
 *
 * 这条命令只做一件事：删除记录。它**不**启动 Gateway、**不**改
 * 状态、**不**动任何版本文件。语义上等同于运维说：
 * "我看过了，可以继续。"
 */
static int ack_operation_command(const char *install_dir)
{
    ota_oplog_record_t record = {0};
    int present = 0;
    int corrupt = 0;

    if (ota_oplog_read(
            install_dir, &record, &present) != 0) {
        /*
         * 解析失败同样应允许解除：记录已经不可读，
         * 再拦住升级只会把人逼向手工 rm。
         */
        corrupt = 1;
    }

    if (present && !corrupt) {
        printf(
            "Acknowledging interrupted operation:\n"
            "  phase      : %s\n"
            "  pid        : %ld\n"
            "  started_at : %lld\n",
            ota_oplog_phase_name(record.phase),
            (long)record.pid,
            record.started_at);
    } else if (corrupt) {
        puts("Acknowledging corrupt operation record");
    } else {
        puts("No pending operation record");
    }

    if (ota_oplog_remove(install_dir) != 0) {
        fprintf(stderr,
                "FAIL: cannot remove operation record: %s\n",
                strerror(errno));
        return 1;
    }

    puts("PASS: operation record cleared; upgrades are allowed");
    return 0;
}

/*
 * 对 OTA 安装目录执行受锁保护的操作。
 * status 使用共享锁；
 * recover 和未来的 upgrade 使用排他锁。
 */
static int run_locked(
    const char *install_dir,
    int exclusive,
    int (*operation)(const char *))
{
    int lockfd = ota_lock_acquire(
        install_dir, exclusive);

    if (lockfd < 0) {
        if (errno == EWOULDBLOCK ||
            errno == EAGAIN) {
            fprintf(stderr,
                    "FAIL: OTA operation already in progress\n");
        } else {
            perror("OTA lock");
        }

        return 1;
    }

    int result = operation(install_dir);

    ota_lock_release(lockfd);

    return result;
}

static int stage_command(
    const char *package_dir,
    const char *install_dir)
{
    int lockfd = ota_lock_acquire(install_dir, 1);

    if (lockfd < 0) {
        perror("OTA stage lock");
        return 1;
    }

    int result = ota_release_stage(
        package_dir, install_dir);

    ota_lock_release(lockfd);
    return result == 0 ? 0 : 1;
}

static int activate_test_command(
    const char *package_dir,
    const char *install_dir)
{
    int lockfd = ota_lock_acquire(install_dir, 1);

    if (lockfd < 0) {
        perror("OTA activation lock");
        return 1;
    }

    int result = ota_activate_test(
        package_dir, install_dir);

    ota_lock_release(lockfd);

    return result == 0 ? 0 : 1;
}

static int upgrade_test_command(
    const char *package_dir,
    const char *install_dir)
{
    int lockfd = ota_lock_acquire(install_dir, 1);

    if (lockfd < 0) {
        /*
         * S4/F10：锁冲突必须给出可读的原因，而不是裸 errno。
         *
         * 原实现用 perror("OTA upgrade lock")，输出是
         *   OTA upgrade lock: Resource temporarily unavailable
         * 运维看到这句无法判断是"另一个 OTA 正在跑"还是
         * "磁盘/权限问题"，也无从知道该怎么办。
         *
         * 并发升级是**预期会发生的**情形（定时任务 + 人工触发），
         * 因此它的提示必须明确到"等前一个结束再试"。
         */
        if (errno == EWOULDBLOCK || errno == EAGAIN) {
            fprintf(stderr,
                    "FAIL: another OTA operation is already in progress\n"
                    "      another process holds .ota.lock in %s\n"
                    "      wait for it to finish, then retry\n",
                    install_dir);
        } else {
            fprintf(stderr,
                    "FAIL: cannot acquire OTA lock: %s\n",
                    strerror(errno));
        }

        return 1;
    }

    int result = ota_upgrade_run_test(
        package_dir, install_dir);

    ota_lock_release(lockfd);

    return result == 0 ? 0 : 1;
}

int main(int argc, char **argv)
{
    if (argc == 3 &&
        strcmp(argv[1], "ack-operation") == 0) {
        return run_locked(
            argv[2], 1, ack_operation_command);
    }

    if (argc == 4 &&
        strcmp(argv[1], "stage") == 0) {
        return stage_command(argv[2], argv[3]);
    }

    if (argc == 4 &&
        strcmp(argv[1], "activate-test") == 0) {
        return activate_test_command(argv[2], argv[3]);
    }

    if (argc == 4 &&
    strcmp(argv[1], "upgrade-test") == 0) {
    return upgrade_test_command(
        argv[2], argv[3]);
    }

    if (argc != 3) {
        print_usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "verify") == 0)
        return ota_verify_package(argv[2]) == 0 ? 0 : 1;

    if (strcmp(argv[1], "status") == 0)
    {
        return run_locked(argv[2], 0, show_status);
    }

    if (strcmp(argv[1], "recover") == 0)
    {
        return run_locked(argv[2], 1, ota_recover_execute);
    }

    print_usage(argv[0]);
    return 1;
}
