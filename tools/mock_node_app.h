#ifndef MOCK_NODE_APP_H
#define MOCK_NODE_APP_H

#include "frame_parser.h"

/*
 * =====================================================================
 * 模拟节点应用层
 *
 * 职责：
 *   1. 周期上报温湿度DATA帧；
 *   2. 接收网关下发的CMD帧，模拟设备操作：
 *      - 解析失败             -> 回发NACK（bad_cmd）
 *      - 参数非法（led非0/1）  -> 模拟操作失败，回发NACK（invalid_led）
 *      - 偶发故障（fail_every）-> 每N条命令模拟一次硬件故障，回发NACK（sim_fault）
 *      - 参数合法且无故障     -> 模拟操作成功，更新LED状态，回发ACK
 *
 * 底层串口I/O由调用方提供文件描述符。
 * =====================================================================
 */

/*
 * 模拟节点应用上下文。
 *
 * fd        ：串口文件描述符（读写同一设备）
 * fail_every：每收到N条合法命令模拟一次设备故障（回NACK sim_fault）；
 *             0表示从不模拟故障
 */
typedef struct
{
    int fd;
    int fail_every;
} mock_node_app_t;

/*
 * 初始化模拟节点应用。
 *
 * @param app 应用上下文
 * @param fd  已打开的串口文件描述符
 */
void mock_node_app_init(mock_node_app_t *app, int fd);

/*
 * 运行模拟节点主循环（无限循环，直到收到信号/出错）。
 *
 * 循环策略（不用sleep，保证命令及时响应）：
 *   1. 高频轮询串口（100ms超时），有网关下发的命令立即解析并回发ACK/NACK；
 *   2. 达到上报时间点（约每秒）才构造并上报一帧温湿度DATA数据。
 *
 * @param app      应用上下文
 * @param running  运行标志；被置0时退出
 *
 * @return 0正常退出
 */
int mock_node_app_run(
    mock_node_app_t *app,
    const volatile int *running
);

#endif
