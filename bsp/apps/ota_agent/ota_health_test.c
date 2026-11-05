
#define _POSIX_C_SOURCE 200809L

#include "ota_health.h"
#include "ota_process.h"

#include <stdio.h>
#include <sys/wait.h>

int main(void)
{
    pid_t pid = -1;
    int status = 0;
    long long stop_ms = 0;
    int ready;
    int negative_ok;
    int stop_result;

    char *const args[] = {
        "./build/smart_gateway",
        "--no-serial",
        "--server",
        "127.0.0.1",
        "--port",
        "9000",
        NULL
    };

    if (ota_process_start(args[0], args, &pid) != 0) {
        perror("start");
        return 1;
    }

    printf("Gateway PID: %ld\n", (long)pid);
    fflush(stdout);

    ready = ota_health_wait_tcp_ready(
        pid, "127.0.0.1", 7000, 3000);

    if (ready == 0)
        puts("PASS: TCP 7000 ready");
    else
        perror("FAIL: TCP readiness");

    /*
     * 负向测试：7001 应未被监听。
     * 执行前应通过 ss 确认。
     */
    negative_ok = ota_health_wait_tcp_ready(
        pid, "127.0.0.1", 7001, 300) != 0;

    if (negative_ok)
        puts("PASS: unavailable port rejected");
    else
        puts("FAIL: unavailable port accepted");

    stop_result = ota_process_stop(
        pid, 3000, &status, &stop_ms);

    printf("Stop duration: %lld ms\n", stop_ms);

    if (ready == 0 &&
        negative_ok &&
        stop_result == OTA_PROCESS_GRACEFUL &&
        WIFEXITED(status) &&
        WEXITSTATUS(status) == 0) {
        puts("PASS: OTA health L1 tests");
        return 0;
    }

    puts("FAIL: OTA health L1 tests");
    return 1;
}
