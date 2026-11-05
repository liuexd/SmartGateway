#ifndef GATEWAY_APP_H
#define GATEWAY_APP_H

#include "frame_parser.h"
#include "serial_port.h"
#include "tcp_client.h"
#include "message_queue.h"
#include "device_manager.h"

#include <time.h>

/*
 * 设备上行回调上下文。
 *
 * Bluetooth Worker / WiFi Worker 都通过它访问：
 * 1. 原有 Gateway 运行状态；
 * 2. M7-6 上行消息队列。
 */
typedef struct
{
    gateway_context_t *gateway_context;

    message_queue_t *upstream_queue;

    device_manager_t *device_manager;

    device_transport_t transport;

    int device_fd;

    char bound_node_id[DEVICE_NODE_ID_SIZE];

}gateway_app_upstream_context_t;

/*
 * 下行（Server -> 网关）回调上下文。
 *
 * 运行在 Server Link Thread，也就是 tcp_fd 的拥有者线程，
 * 因此路由失败时可以直接同步回一条 NACK 给 Server。
 */
typedef struct
{
    gateway_context_t *gateway_context;

    device_manager_t *device_manager;

}gateway_app_downlink_context_t;


/*
 * =====================================================================
 * 网关应用层
 *
 * 职责：
 *   1. 维护运行上下文（gateway_context_t）；
 *   2. 解析命令行参数（--serial/--baud/--server/--port）；
 *   3. 将设备 DATA/ACK/NACK 转为上行消息，路由 Server 命令；
 *   4. 管理TCP自动重连。
 *
 * 底层I/O（串口读取、主循环）见 gateway_loop.h。
 * =====================================================================
 */

/*
 * 初始化网关上下文。
 *
 * @param context   输出上下文（调用前无需清零）
 * @param server_ip 服务器地址，例如 "127.0.0.1"
 * @param server_port 服务器端口
 */
void gateway_app_init(
    gateway_context_t *context,
    const char *server_ip,
    unsigned short server_port
);

/*
 * 解析命令行参数。
 *
 * 支持：
 *   --serial <device> 或 --no-serial  选择串口模式
 *   --baud <rate>      波特率（可选，默认9600）
 *   --server <ip>      服务器地址（可选，默认127.0.0.1）
 *   --port <port>      服务器端口（可选，默认9000）
 *   --help / -h        打印帮助
 *
 * @param argc,argv  命令行参数
 * @param serial_device 输出：串口设备路径（未提供时为NULL）
 * @param baud_rate  输出：波特率
 * @param context    用于接收 --server/--port 设置
 *
 * @return 0成功；1参数错误/打印帮助后退出
 */
int gateway_app_parse_args(
    int argc,
    char *argv[],
    const char **serial_device,
    int *baud_rate,
    gateway_context_t *context
);

/*
 * 尝试连接TCP服务器（带重试节流）。
 *
 * 未连接时调用；已连接则直接返回。
 * 连接失败时记录 tcp_connect_error，并设置 next_tcp_retry。
 *
 * 连接本身是有界的（见 tcp_client_connect_stoppable）：
 *   - running 非 NULL 时，置 0 可立即中断正在进行的 connect；
 *   - 超时由 TCP_CLIENT_CONNECT_TIMEOUT_MS 控制。
 *
 * @param context 网关上下文
 * @param running Gateway 全局运行标志，可为 NULL
 */
void gateway_app_try_tcp_connect(
    gateway_context_t *context,
    const volatile int *running
);

/*
 * Bluetooth Worker 的帧解析回调：解码 DATA、ACK 或 NACK，绑定或刷新
 * 设备，并将 JSON 放入上行队列。入队可能因队列已满而阻塞；是否已
 * 连接 Server 不影响入队。Server Link Worker 负责后续发送。
 * frame 是已通过 CRC 校验的帧；user_data 为该 worker 持有的
 * gateway_app_upstream_context_t *，其共享队列和设备表须保持有效。
 */
void gateway_app_on_frame(
    const parsed_frame_t *frame,
    void *user_data
);

/*
 * 处理从TCP服务器收到的一行JSON（控制命令）。
 *
 * 流程：
 *   1. message_json_decode_command() 解码JSON -> frame_command_t；
 *   2. device_manager_enqueue_command() 按 target_node 投递到该设备的命令队列。
 *
 * 注意：本函数运行在 Server Link Thread 中，
 * 不直接操作设备串口或 WiFi socket。真正的下行发送由
 * 拥有该链路的设备 worker（bluetooth_worker / wifi_client_worker）
 * 在获得连接所有权的前提下完成。路由失败时，此线程可以使用自己
 * 拥有的北向 TCP 连接直接发送 NACK。
 *
 * 这样做的原因：串口 fd 的所有权唯一，只有 bluetooth_worker 会
 * open/write/close 它；否则会在断线时出现"已关闭的fd又被写入"的竞态。
 *
 * 该函数可作为 line_parser_feed() 的回调，
 * user_data 必须是 gateway_app_downlink_context_t*。
 *
 * @param line      完整JSON行（不含'\n'，已去除结尾的'\r'）
 * @param length    JSON长度
 * @param user_data 必须是 gateway_app_downlink_context_t*
 */
void gateway_app_on_tcp_line(
    const char *line,
    size_t length,
    void *user_data
);

/*
 * WiFi Worker 的行解析回调：处理节点的 DATA、ACK、NACK JSON，
 * 刷新设备并将消息加入上行队列，队列满时可能阻塞。
 * line/length 指向完整行；user_data 为 gateway_app_upstream_context_t *。
 */
void gateway_app_on_wifi_line(
    const char*line,
    size_t length,
    void *user_data
);

/*
 * 打印退出时的统计信息。
 *
 * @param context 网关上下文
 * @param parser  帧解析器（用于读取parser.stats）
 */
void gateway_app_print_stats(
    const gateway_context_t *context,
    const frame_parser_t *parser
);

/*
 * 关闭TCP连接（如果存在）。
 *
 * @param context 网关上下文
 */
void gateway_app_close(gateway_context_t *context);

/* 设备 Worker 将 node_id、sequence 和 error 封装为上行 NACK；
 * upstream_queue 须保持有效，队列满时可能阻塞；无返回值。 */
void gateway_app_enqueue_nack(
    message_queue_t *upstream_queue,
    const char *node_id,
    uint32_t sequence,
    const char *error
);

#endif
