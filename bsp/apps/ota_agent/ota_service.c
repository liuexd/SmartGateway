#define _GNU_SOURCE

#include "ota_service.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "service_ipc.h"

/*
 * 运行目录。
 *
 * 与 supervisor 的约定：<install_dir>/run 下放 supervisor.sock
 * 与 service.pid。
 *
 * 为什么用 install_dir 而不是硬编码 /opt/smartgateway：
 *   x86 集成测试需要在临时目录里跑完整流程，硬编码会让测试
 *   互相干扰（共享同一个 socket）。用 install_dir 也让"同一台
 *   机器上跑两套安装"成为可能，虽然板端不需要。
 */
static int run_dir_of(
    const char *install_dir, char *out, size_t cap)
{
    if (install_dir == NULL || install_dir[0] == '\0')
        return -1;

    int n = snprintf(out, cap, "%s/run", install_dir);

    if (n < 0 || (size_t)n >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

const char *ota_service_state_name(ota_service_state_t state)
{
    switch (state) {
    case OTA_SERVICE_STOPPED:
        return "STOPPED";
    case OTA_SERVICE_RUNNING:
        return "RUNNING";
    default:
        return "UNKNOWN";
    }
}

/*
 * 通用调用封装。
 *
 * 把"通信失败"统一成 -1 + errno，让上层只处理一种错误形态。
 */
static int call(
    const char *install_dir,
    service_ipc_request_t request,
    int timeout_ms,
    service_ipc_reply_t *reply_out)
{
    char run_dir[PATH_MAX];

    if (run_dir_of(install_dir, run_dir, sizeof(run_dir)) != 0)
        return -1;

    return service_ipc_call(run_dir, request, timeout_ms, reply_out);
}

int ota_service_probe(const char *install_dir)
{
    service_ipc_reply_t reply;

    if (call(install_dir, SERVICE_IPC_STATUS, 5000, &reply) != 0)
        return -1;

    /*
     * 能收到一个合法响应就说明 supervisor 活着。
     *
     * 不要求 result 必须是 OK：即使 Gateway 状态有问题，
     * supervisor 本身可用就够了 —— probe 的语义是
     * "这个 IPC 通道能不能用"。
     */
    if (reply.result != SERVICE_IPC_OK &&
        reply.result != SERVICE_IPC_NOT_RUNNING) {
        errno = EINVAL;
        return -1;
    }

    return 0;
}

int ota_service_query(
    const char *install_dir,
    ota_service_state_t *state_out,
    pid_t *pid_out)
{
    service_ipc_reply_t reply;

    if (state_out != NULL)
        *state_out = OTA_SERVICE_UNKNOWN;

    if (pid_out != NULL)
        *pid_out = -1;

    if (call(install_dir, SERVICE_IPC_STATUS, 5000, &reply) != 0)
        return -1;

    if (reply.result != SERVICE_IPC_OK) {
        if (state_out != NULL)
            *state_out = OTA_SERVICE_UNKNOWN;

        errno = EINVAL;
        return -1;
    }

    /*
     * 判定依据是 detail 字段，而不是 pid != 0。
     *
     * 用 pid 判断看似更"客观"，但 supervisor 在启动失败的中间态
     * 可能已经有 pid 而 detail 为错误。以显式状态词为准，
     * 并且对不认识的状态词返回 UNKNOWN 而不是猜一个。
     */
    if (strcmp(reply.detail, "running") == 0) {
        if (state_out != NULL)
            *state_out = OTA_SERVICE_RUNNING;

        if (pid_out != NULL)
            *pid_out = reply.pid;

        return 0;
    }

    if (strcmp(reply.detail, "stopped") == 0) {
        if (state_out != NULL)
            *state_out = OTA_SERVICE_STOPPED;

        return 0;
    }

    /*
     * 不认识的状态词：可能是协议版本不一致。
     * 绝不猜成 STOPPED —— 那会导致启动第二个实例。
     */
    if (state_out != NULL)
        *state_out = OTA_SERVICE_UNKNOWN;

    errno = EINVAL;
    return -1;
}

int ota_service_start(
    const char *install_dir,
    const char *upstream_server,
    int upstream_port,
    pid_t *pid_out)
{
    service_ipc_reply_t reply;

    if (pid_out != NULL)
        *pid_out = -1;

    service_ipc_call_t call = { .request = SERVICE_IPC_START };

    if (upstream_server != NULL && upstream_server[0] != '\0' &&
        upstream_port > 0) {
        call.upstream_port = upstream_port;
        snprintf(call.upstream_server,
                 sizeof(call.upstream_server), "%s", upstream_server);
    }

    char run_dir[PATH_MAX];

    if (run_dir_of(install_dir, run_dir, sizeof(run_dir)) != 0)
        return -1;

    if (service_ipc_call_ex(
            run_dir, &call, 10000, &reply) != 0)
        return -1;

    if (reply.result == SERVICE_IPC_OK) {
        if (pid_out != NULL)
            *pid_out = reply.pid;

        return 0;
    }

    if (reply.result == SERVICE_IPC_ALREADY) {
        if (pid_out != NULL)
            *pid_out = reply.pid;

        return OTA_SERVICE_ERR_ALREADY;
    }

    fprintf(stderr,
            "FAIL: supervisor could not start the gateway: %s\n",
            reply.detail);

    errno = EIO;
    return -1;
}

int ota_service_stop(const char *install_dir)
{
    service_ipc_reply_t reply;

    /*
     * 超时 20s，比 supervisor 自己的 stop_timeout（默认 5s）
     * 加 SIGKILL 宽限（2s）留足余量。
     *
     * 若客户端超时早于 supervisor 的等待，客户端会报"超时"而
     * supervisor 还在正常工作，运维将被误导为"停不掉"。
     */
    if (call(install_dir, SERVICE_IPC_STOP, 20000, &reply) != 0)
        return -1;

    if (reply.result == SERVICE_IPC_OK)
        return 0;

    if (reply.result == SERVICE_IPC_NOT_RUNNING)
        return OTA_SERVICE_ERR_NOT_RUNNING;

    /*
     * FAILED：进程确实没停下来。
     *
     * 这一条必须能被上层区分出来——它与"通信失败"不同：
     * 通信失败可能是 supervisor 挂了，进程状态未知；
     * 而 FAILED 意味着 supervisor 明确报告"停不掉"，
     * 通常是不间断睡眠（D 状态）。
     */
    fprintf(stderr,
            "FAIL: supervisor could not stop the gateway: %s\n",
            reply.detail);

    errno = EBUSY;
    return -1;
}
