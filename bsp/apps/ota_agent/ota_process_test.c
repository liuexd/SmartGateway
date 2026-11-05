
#define _POSIX_C_SOURCE 200809L

#include "ota_process.h"

#include <errno.h>
#include <stdio.h>
#include <sys/wait.h>
#include <time.h>

int main(void)
{
    pid_t pid;
    int status;
    int stop_result;
    long long elapsed_ms;

    char *const args[] = {
        "./build/smart_gateway",
        "--no-serial",
        "--server",
        "127.0.0.1",
        "--port",
        "9000",
        NULL
    };

    if (ota_process_start(
            args[0], args, &pid) != 0) {
        perror("ota_process_start");
        return 1;
    }

    printf("Gateway child PID: %ld\n", (long)pid);
    fflush(stdout);

    struct timespec delay = {
        .tv_sec = 2,
        .tv_nsec = 0
    };

    while (nanosleep(&delay, &delay) != 0 &&
           errno == EINTR) {
    }

    stop_result = ota_process_stop(
        pid,
        3000,
        &status,
        &elapsed_ms
    );

    printf("Stop result: %d\n", stop_result);
    printf("Stop duration: %lld ms\n", elapsed_ms);

    if (stop_result == OTA_PROCESS_GRACEFUL &&
        WIFEXITED(status) &&
        WEXITSTATUS(status) == 0) {

        puts("PASS: graceful Gateway lifecycle");
        return 0;
    }

    if (stop_result == OTA_PROCESS_FORCED)
        puts("FAIL: Gateway required SIGKILL");
    else if (stop_result == OTA_PROCESS_ALREADY_EXITED)
        puts("FAIL: Gateway exited before stop request");
    else
        puts("FAIL: unexpected process result");

    return 1;
}
