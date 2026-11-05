
#ifndef OTA_PROCESS_H
#define OTA_PROCESS_H

#include <sys/types.h>

/* ota_process_stop() 的返回值 */
enum {
    OTA_PROCESS_GRACEFUL = 0,
    OTA_PROCESS_FORCED = 1,
    OTA_PROCESS_ALREADY_EXITED = 2
};

/*
 * 启动 Gateway 子进程。
 * 返回 0：execv 成功。
 * 返回 -1：fork/execv 等操作失败，errno 有效。
 *
 * 注意：execv 成功不代表 Gateway 业务启动成功。
 */
int ota_process_start(
    const char *executable,
    char *const argv[],
    pid_t *pid_out
);

/*
 * 停止并回收本进程创建的子进程。
 *
 * timeout_ms：SIGTERM 优雅退出超时时间。
 * wait_status：waitpid 原始退出状态。
 * elapsed_ms：从发送停止请求到回收完成的耗时。
 *
 * 返回：
 *   0 = SIGTERM 后退出
 *   1 = 超时，使用 SIGKILL
 *   2 = 调用前已经退出
 *  -1 = 系统调用失败
 */
int ota_process_stop(
    pid_t pid,
    int timeout_ms,
    int *wait_status,
    long long *elapsed_ms
);

#endif

