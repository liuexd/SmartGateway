#include "mock_node_app.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int g_running = 1;

int main(int argc, char *argv[])
{
    const char *device = "/tmp/ttyNODE";
    int fail_every = 0;
    int fd;
    mock_node_app_t app;

    /*
     * 可以从命令行指定设备路径：
     *
     * ./build/mock_node /tmp/ttyNODE [fail_every]
     *
     * fail_every：每N条合法命令模拟一次设备故障（回NACK sim_fault），
     *             省略或为0表示从不模拟故障。
     */
    if (argc >= 2)
    {
        device = argv[1];
    }

    if (argc >= 3)
    {
        fail_every = atoi(argv[2]);

        if (fail_every < 0)
        {
            fail_every = 0;
        }
    }

    /*
     * O_RDWR：
     * 以可读写方式打开。
     *
     * O_NOCTTY：
     * 不让这个串口成为当前程序的控制终端。
     */
    fd = open(device, O_RDWR | O_NOCTTY);

    if (fd < 0)
    {
        perror("open mock node device");
        return 1;
    }

    printf("Mock node opened: %s\n", device);

    if (fail_every > 0)
    {
        printf("Simulated fault: every %d command(s)\n", fail_every);
    }

    mock_node_app_init(&app, fd);
    app.fail_every = fail_every;

    mock_node_app_run(&app, &g_running);

    close(fd);
    return 0;
}