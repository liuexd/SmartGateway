/* 模块 bluetooth_worker：独占串口并处理蓝牙帧、命令发送和串口重连。 */
#ifndef BLUETOOTH_WORKER_H
#define BLUETOOTH_WORKER_H

#include "gateway_app.h"
#include "frame_parser.h"
#include "device_manager.h"

/*
 * 主循环创建并持有此上下文，直到 Bluetooth Worker join 完成。
 * Worker 独占串口；parser、队列和设备表在整个线程生命周期内有效。
 */
typedef struct
{
    const char *serial_device;
    int baud_rate;

    gateway_context_t *gateway_context;
    frame_parser_t *parser;

    message_queue_t *upstream_queue;

    device_manager_t *device_manager;

    const volatile int *running;

} bluetooth_worker_context_t;

/* pthread 入口；arg 指向 bluetooth_worker_context_t，退出时返回 NULL。 */
void *bluetooth_worker(void *arg);

#endif