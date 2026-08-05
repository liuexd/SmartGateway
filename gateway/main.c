#define _POSIX_C_SOURCE 200809L

#include "frame_parser.h"
#include "gateway_app.h"
#include "gateway_loop.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

/*
 * 使用sig_atomic_t，保证信号处理函数修改该变量时安全。
 */
static volatile sig_atomic_t g_running = 1;

/*
 * 收到Ctrl+C或者systemd停止信号时，
 * 只修改运行标志，不在信号函数中做复杂操作。
 */
static void handle_signal(int signal_number)
{
    (void)signal_number;

    g_running = 0;
}

/*
 * 安装Ctrl+C和终止信号处理函数。
 */
static int install_signal_handlers(void)
{
    struct sigaction action;

    memset(&action, 0, sizeof(action));

    action.sa_handler = handle_signal;

    sigemptyset(&action.sa_mask);

    if (sigaction(SIGINT, &action, NULL) != 0)
    {
        return -1;
    }

    if (sigaction(SIGTERM, &action, NULL) != 0)
    {
        return -1;
    }

    return 0;
}

int main(int argc, char *argv[])
{
    const char *serial_device = NULL;
    int baud_rate = 9600;

    frame_parser_t parser;
    gateway_context_t context;
    int result;

    /*
     * 应用层默认配置。
     */
    gateway_app_init(
        &context,
        "127.0.0.1",
        9000
    );

    /*
     * 解析命令行参数。
     */
    result = gateway_app_parse_args(
        argc,
        argv,
        &serial_device,
        &baud_rate,
        &context
    );

    if (result != 0)
    {
        fprintf(
            stdout,
            "Usage: %s --serial <device> [--baud <rate>] "
            "--server <server_ip> --port <server_port>\n",
            argv[0]
        );

        fprintf(
            stdout,
            "Example: %s --serial /tmp/ttyGW --baud 9600 "
            "--server 127.0.0.1 --port 9000\n",
            argv[0]
        );

        return 1;
    }

    /*
     * 设备路径必须从命令行提供，不能写死。
     */
    if (serial_device == NULL)
    {
        fprintf(stderr, "--serial is required\n");

        fprintf(
            stdout,
            "Usage: %s --serial <device> [--baud <rate>] "
            "--server <server_ip> --port <server_port>\n",
            argv[0]
        );

        return 1;
    }

    if (install_signal_handlers() != 0)
    {
        perror("sigaction");
        return 1;
    }

    frame_parser_init(&parser);

    /*
     * 进入串口主循环。
     */
    gateway_loop_run(
        serial_device,
        baud_rate,
        &context,
        &parser,
        &g_running
    );

    /*
     * Ctrl+C正常退出后打印所有统计。
     */
    gateway_app_print_stats(&context, &parser);
    gateway_app_close(&context);

    return 0;
}
