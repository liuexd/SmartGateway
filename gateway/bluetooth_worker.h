#ifndef  BLUETOOTH_WORKER_H
#define  BLUETOOTH_WORKER_H

#include "gateway_app.h"
#include "frame_parser.h"

/*
 *Bluetooth Worker 启动参数
 *M7-5 初期仍复用现有的gateway_context，后续改成message queue和frame_parser。
 *后续只迁移串口I/O的执行位置，不改变协议逻辑
 */

typedef struct
{
    const char *serial_device;
    int baud_rate;

    gateway_context_t *gateway_context;
    frame_parser_t *parser;

    message_queue_t *upstream_queue;

    const volatile int *running;

} bluetooth_worker_context_t;

void *bluetooth_worker(void *arg);

#endif