#define _GNU_SOURCE

#include "service_supervisor.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "service_ipc.h"

/*
 * service_supervisor - 命令行入口
 *
 * 子命令：
 *   run         前台运行（供 init 脚本使用，配合 start-stop-daemon -b）
 *   start       请求 supervisor 启动 Gateway
 *   stop        请求 supervisor 停止 Gateway
 *   restart     先 stop 再 start
 *   status      查询 Gateway 状态
 *   reload      请求重载配置
 *   init-config 首次部署时写出默认配置（不覆盖已有文件）
 *
 * 为什么 CLI 与守护进程在同一个二进制：
 *   板端部署只有一份文件要拷贝，减少"版本不一致"的可能。
 *   （CLI 与 daemon 分离时，很容易出现"新的 CLI 配旧的 daemon"。）
 *
 * 为什么 CLI 走 IPC 而不是直接操作 pidfile：
 *   直接操作 pidfile 等于绕过了 supervisor 的所有权。那正是
 *   L3/L6 的成因——OTA 曾经就是这么做的。
 */

#define DEFAULT_ROOT "/opt/smartgateway"

static void usage(const char *program)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s run [--root DIR]\n"
            "  %s start|stop|restart|status|reload [--root DIR]\n"
            "  %s init-config [--root DIR] [--executable PATH]\n"
            "\n"
            "Options:\n"
            "  --root DIR         installation root (default %s)\n"
            "  --executable PATH  gateway binary for init-config\n"
            "  --foreground       do not detach (only for 'run')\n",
            program, program, program, DEFAULT_ROOT);
}

static int request_name_to_enum(
    const char *name, service_ipc_request_t *out)
{
    if (strcmp(name, "start") == 0) {
        *out = SERVICE_IPC_START;
        return 0;
    }

    if (strcmp(name, "stop") == 0) {
        *out = SERVICE_IPC_STOP;
        return 0;
    }

    if (strcmp(name, "status") == 0) {
        *out = SERVICE_IPC_STATUS;
        return 0;
    }

    if (strcmp(name, "reload") == 0) {
        *out = SERVICE_IPC_RELOAD;
        return 0;
    }

    return -1;
}

/*
 * 打印 IPC 调用的失败原因。
 *
 * 区分 ENOENT 与 ECONNREFUSED 很重要：
 *   ENOENT       socket 不存在 -> supervisor 没装、没启动
 *   ECONNREFUSED 文件在但无人监听 -> supervisor 挂了（残留文件）
 * 两者的处置方式完全不同，不能笼统地报"连接失败"。
 *
 * run_dir 必须由调用方传入，不能在这里写死 /opt/smartgateway：
 * --root 允许指向任意安装目录，报错时若显示一个与实际不符的
 * 路径，会把人引到错误的排查方向（实测踩过）。
 */
static int report_ipc_failure(const char *run_dir, int err)
{
    fprintf(stderr, "FAIL: cannot reach service supervisor\n");

    if (err == ENOENT) {
        fprintf(stderr,
                "      %s/supervisor.sock does not exist\n"
                "      the supervisor is not running\n",
                run_dir);
    } else if (err == ECONNREFUSED) {
        fprintf(stderr,
                "      socket exists but nobody is listening\n"
                "      the supervisor may have crashed; restart init script\n");
    } else if (err == ETIMEDOUT) {
        fprintf(stderr,
                "      supervisor did not respond in time\n"
                "      it may be busy stopping a process\n");
    } else {
        fprintf(stderr, "      %s\n", strerror(err));
    }

    return 1;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    const char *command = argv[1];
    const char *root = DEFAULT_ROOT;
    const char *executable = NULL;
    int foreground = 0;

    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "--root") == 0 && i + 1 < argc) {
            root = argv[++i];
        } else if (strcmp(argv[i], "--executable") == 0 &&
                   i + 1 < argc) {
            executable = argv[++i];
        } else if (strcmp(argv[i], "--foreground") == 0) {
            foreground = 1;
        } else {
            fprintf(stderr, "FAIL: unknown option: %s\n", argv[i]);
            usage(argv[0]);
            return 1;
        }
    }

    /* 运行目录与配置文件路径 */
    char run_dir[PATH_MAX];
    char config_path[PATH_MAX];

    int n = snprintf(run_dir, sizeof(run_dir), "%s/run", root);

    if (n < 0 || (size_t)n >= sizeof(run_dir)) {
        fprintf(stderr, "FAIL: root path too long\n");
        return 1;
    }

    n = snprintf(
        config_path, sizeof(config_path), "%s/%s",
        root, SERVICE_CONFIG_NAME);

    if (n < 0 || (size_t)n >= sizeof(config_path)) {
        fprintf(stderr, "FAIL: config path too long\n");
        return 1;
    }

    if (strcmp(command, "run") == 0)
        return service_supervisor_run(
                   run_dir, config_path, foreground) == 0 ? 0 : 1;

    if (strcmp(command, "init-config") == 0) {
        if (executable == NULL) {
            n = snprintf(
                config_path, sizeof(config_path),
                "%s/%s", root, SERVICE_CONFIG_NAME);

            fprintf(stderr,
                    "FAIL: --executable is required\n"
                    "      e.g. %s init-config --executable %s/current/smart_gateway\n",
                    argv[0], root);
            return 1;
        }

        if (service_config_write_default(
                config_path, executable) != 0) {
            if (errno == EEXIST) {
                printf("NOTICE: config already exists: %s\n", config_path);
                return 0;
            }

            fprintf(stderr,
                    "FAIL: cannot write config: %s\n",
                    strerror(errno));
            return 1;
        }

        printf("PASS: wrote %s\n", config_path);
        return 0;
    }

    service_ipc_request_t request;

    if (strcmp(command, "restart") == 0) {
        /*
         * restart 由客户端组合两个请求，而不是让 supervisor
         * 认识一个 RESTART 命令。
         *
         * 原因：restart 在语义上是"先确保停下，再启动"，
         * 客户端可以精确知道每一步的结果。若做成单个 IPC 命令，
         * "停止成功但启动失败"这种中间状态就很难表达。
         */
        service_ipc_reply_t reply;

        if (service_ipc_call(
                run_dir, SERVICE_IPC_STOP, 15000, &reply) != 0)
            return report_ipc_failure(run_dir, errno);

        printf("stop: %s %s\n",
               service_ipc_result_name(reply.result),
               reply.detail);

        if (service_ipc_call(
                run_dir, SERVICE_IPC_START, 15000, &reply) != 0)
            return report_ipc_failure(run_dir, errno);

        printf("start: %s %s (pid=%ld)\n",
               service_ipc_result_name(reply.result),
               reply.detail,
               (long)reply.pid);

        return reply.result == SERVICE_IPC_OK ||
               reply.result == SERVICE_IPC_ALREADY ? 0 : 1;
    }

    if (request_name_to_enum(command, &request) != 0) {
        fprintf(stderr, "FAIL: unknown command: %s\n", command);
        usage(argv[0]);
        return 1;
    }

    service_ipc_reply_t reply;

    /*
     * 超时分级：
     *   STATUS/RELOAD 立即返回 -> 5s 足够
     *   START         需要 fork/exec -> 10s
     *   STOP          需要等 SIGTERM 宽限期（默认 5s）+ SIGKILL -> 15s
     *
     * 给 STOP 留出比 stop_timeout_ms 更大的余量：若客户端超时早于
     * supervisor 的等待，客户端会报"超时"而 supervisor 还在正常
     * 工作，运维将被误导。
     */
    int timeout_ms = 5000;

    if (request == SERVICE_IPC_START)
        timeout_ms = 10000;
    else if (request == SERVICE_IPC_STOP)
        timeout_ms = 15000;

    if (service_ipc_call(
            run_dir, request, timeout_ms, &reply) != 0)
        return report_ipc_failure(run_dir, errno);

    if (request == SERVICE_IPC_STATUS) {
        printf("status: %s pid=%ld starttime=%llu\n",
               reply.detail,
               (long)reply.pid,
               reply.starttime);

        return reply.result == SERVICE_IPC_OK ? 0 : 1;
    }

    printf("%s: %s %s\n",
           command,
           service_ipc_result_name(reply.result),
           reply.detail);

    switch (reply.result) {
    case SERVICE_IPC_OK:
    case SERVICE_IPC_ALREADY:
        return 0;
    case SERVICE_IPC_NOT_RUNNING:
        /*
         * stop 一个本来就没跑的服务，语义上是"目标状态已达成"。
         * 返回 0 而不是失败——init 脚本在 shutdown 路径上会调用
         * stop，此时服务可能早已退出，报失败会造成误报警。
         */
        return 0;
    default:
        return 1;
    }
}
