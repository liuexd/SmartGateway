/* 模块 main：程序入口、命令行初始化、信号处理和退出统计。 */
#define _POSIX_C_SOURCE 200809L

#include "frame_parser.h"
#include "gateway_app.h"
#include "gateway_loop.h"

#include <signal.h>
#include <stdio.h>
#include <string.h>

/* 这个宏的作用是允许编译时指定 Gateway 的版本。没有提供版本参数时，默认使用 dev。 */
#ifndef SMART_GATEWAY_VERSION
#define SMART_GATEWAY_VERSION "dev"
#endif
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
    if (argc == 2 && strcmp(argv[1], "--version") == 0)
    {
        printf("smart_gateway %s\n", SMART_GATEWAY_VERSION);
        return 0;
    }

    const char *serial_device = NULL;
    int baud_rate = 9600;

    frame_parser_t parser;
    gateway_context_t context;
    int result;

    /*
     * 应用层默认配置。
     */
    gateway_app_init(&context, "127.0.0.1", 9000);

    /*
     * 解析命令行参数。
     */
    result = gateway_app_parse_args(argc, argv, &serial_device, &baud_rate, &context);

    if (result != 0)
    {
        fprintf(stdout,
                "Usage: %s (--serial <device> | --no-serial) [--baud <rate>] "
                "--server <server_ip> --port <server_port>\n",
                argv[0]);

        fprintf(stdout,
                "Example: %s --serial /tmp/ttyGW --baud 9600 "
                "--server 127.0.0.1 --port 9000\n",
                argv[0]);

        return 1;
    }

    if (install_signal_handlers() != 0)
    {
        perror("sigaction");
        return 1;
    }

    frame_parser_init(&parser);

    /*
     * 启动网关循环：监听 WiFi，管理 Server Link 和可选的蓝牙 worker。
     */
    gateway_loop_run(serial_device, baud_rate, &context, &parser, &g_running);

    /*
     * 主循环完成线程回收后打印统计，最后关闭剩余应用资源。
     */
    gateway_app_print_stats(&context, &parser);
    gateway_app_close(&context);

    return 0;
}
