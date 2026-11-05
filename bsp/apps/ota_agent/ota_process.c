
#define _GNU_SOURCE

#include "ota_process.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* 使用单调时钟，避免系统时间调整影响计时 */
static long long monotonic_ms(void)
{
    struct timespec ts;

    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;

    return (long long)ts.tv_sec * 1000LL +
           ts.tv_nsec / 1000000LL;
}

static int wait_child(
    pid_t pid,
    int *status,
    int options)
{
    pid_t result;

    do {
        result = waitpid(pid, status, options);
    } while (result < 0 && errno == EINTR);

    if (result < 0)
        return -1;

    return result == 0 ? 0 : 1;
}

int ota_process_start(
    const char *executable,
    char *const argv[],
    pid_t *pid_out)
{
    int error_pipe[2];
    pid_t pid;
    int child_error = 0;
    size_t received = 0;

    if (!executable || !*executable ||
        !argv || !argv[0] || !pid_out) {
        errno = EINVAL;
        return -1;
    }

    *pid_out = -1;

    /*
     * execv 成功时，FD_CLOEXEC 自动关闭写端，
     * 父进程读到 EOF。
     *
     * execv 失败时，子进程通过管道返回 errno。
     */
    if (pipe2(error_pipe, O_CLOEXEC) != 0)
        return -1;

    pid = fork();

    if (pid < 0) {
        int saved_errno = errno;
        close(error_pipe[0]);
        close(error_pipe[1]);
        errno = saved_errno;
        return -1;
    }

    if (pid == 0) {
        int exec_error;

        close(error_pipe[0]);//关闭读端

        execv(executable, argv);

        exec_error = errno;

        /* fork 后失败路径仅使用适合子进程的系统调用 */
        (void)write(
            error_pipe[1],
            &exec_error,
            sizeof(exec_error));

        _exit(127);
    }

    close(error_pipe[1]);

    while (received < sizeof(child_error)) {
        ssize_t n = read(
            error_pipe[0],
            (char *)&child_error + received,
            sizeof(child_error) - received);

        if (n < 0 && errno == EINTR)
            continue;

        if (n < 0) {
            int saved_errno = errno;
            close(error_pipe[0]);
            (void)kill(pid, SIGKILL);
            (void)wait_child(pid, NULL, 0);
            errno = saved_errno;
            return -1;
        }

        if (n == 0)
            break;

        received += (size_t)n;
    }

    close(error_pipe[0]);

    if (received == 0) {
        *pid_out = pid;
        return 0;
    }

    if (received != sizeof(child_error)) {
        (void)kill(pid, SIGKILL);
        child_error = EIO;
    }

    (void)wait_child(pid, NULL, 0);
    errno = child_error;
    return -1;
}

int ota_process_stop(
    pid_t pid,
    int timeout_ms,
    int *wait_status,
    long long *elapsed_ms)
{
    int status = 0;
    int result;
    long long start_ms;
    long long now_ms;

    if (pid <= 1 || timeout_ms < 0 ||
        !wait_status || !elapsed_ms) {
        errno = EINVAL;
        return -1;
    }

    *elapsed_ms = 0;
    *wait_status = 0;

    /*
     * 必须是当前进程创建且尚未回收的子进程。
     * 避免根据进程名称误杀其他 Gateway。
     */
    result = wait_child(pid, &status, WNOHANG);

    if (result < 0)
        return -1;

    if (result == 1) {
        *wait_status = status;
        return OTA_PROCESS_ALREADY_EXITED;
    }

    start_ms = monotonic_ms();

    if (start_ms < 0)
        return -1;

    if (kill(pid, SIGTERM) != 0 && errno != ESRCH)
        return -1;

    for (;;) {
        result = wait_child(pid, &status, WNOHANG);

        if (result < 0)
            return -1;

        now_ms = monotonic_ms();

        if (now_ms < 0)
            return -1;

        if (result == 1) {
            *wait_status = status;
            *elapsed_ms = now_ms - start_ms;
            return OTA_PROCESS_GRACEFUL;
        }

        if (now_ms - start_ms >= timeout_ms)
            break;

        struct timespec pause = {
            .tv_sec = 0,
            .tv_nsec = 20 * 1000 * 1000
        };

        while (nanosleep(&pause, &pause) != 0 &&
               errno == EINTR) {
        }
    }

    /* 超过限定时间，强制终止并回收子进程 */
    if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
        return -1;

    if (wait_child(pid, &status, 0) != 1)
        return -1;

    now_ms = monotonic_ms();

    if (now_ms < 0)
        return -1;

    *wait_status = status;
    *elapsed_ms = now_ms - start_ms;

    return OTA_PROCESS_FORCED;
}
