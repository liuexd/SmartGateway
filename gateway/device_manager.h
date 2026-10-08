#ifndef DEVICE_MANAGER_H
#define DEVICE_MANAGER_H

#include "frame.h"

#include <pthread.h>
#include <stddef.h>
#include <time.h>

/*
 * 设备管理器：统一登记所有已接入的节点。
 *
 * 网关有两条异构的南向链路（蓝牙串口 / WiFi TCP），
 * 但北向下行命令只知道目标节点的 node_id。
 * 本模块维护 node_id -> 通信资源的映射，
 * 让命令能够路由到正确的链路。
 *
 * 并发说明：
 *   bluetooth_worker、各个 wifi_client_worker、
 *   server_link_worker 都会访问同一份 manager，
 *   因此所有接口内部都用 mutex 保护。
 */

/*
 * 单条链路可容纳的设备上限。
 *
 * 用于静态分配 devices 数组，避免运行期动态内存。
 */
#define DEVICE_MANAGER_MAX_DEVICES 16

/*
 * node_id 字段长度，需与协议中节点标识的长度约定保持一致。
 */
#define DEVICE_NODE_ID_SIZE 32

/*
 * 接口返回值。
 *
 * OK        ：操作成功
 * ERROR     ：参数非法或内部错误（如 mutex 初始化失败）
 * NOT_FOUND ：按 node_id 查找不到对应设备
 * EXISTS    ：注册时该 node_id 已存在（重复注册）
 * FULL      ：注册表已满，无法再接纳新设备
 */
#define DEVICE_MANAGER_QUEUE_EMPTY    1
#define DEVICE_MANAGER_OK          0
#define DEVICE_MANAGER_ERROR      -1
#define DEVICE_MANAGER_NOT_FOUND  -2
#define DEVICE_MANAGER_EXISTS     -3
#define DEVICE_MANAGER_FULL       -4
#define DEVICE_MANAGER_CONFLICT   -5
#define DEVICE_MANAGER_OFFLINE       -6
#define DEVICE_MANAGER_QUEUE_FULL    -7
#define DEVICE_MANAGER_BUFFER_TOO_SMALL -8

#define DEVICE_COMMAND_QUEUE_CAPACITY 8


/*
 * 设备接入网关所用的物理链路类型。
 *
 * 决定下行命令使用哪种方式发送：
 * 蓝牙走共享串口，WiFi 走该节点独占的 TCP 连接。
 */
typedef enum
{
    DEVICE_TRANSPORT_BLUETOOTH = 0,   /* 串口链路（多个节点共用串口） */
    DEVICE_TRANSPORT_WIFI             /* WiFi 链路（每节点独占一条 TCP 连接） */

} device_transport_t;


/*
 * 单个已注册设备（节点）的登记信息。
 *
 * 这是设备的"身份 + 可达性"快照，
 * 供下行命令寻址和在线状态展示使用。
 */
typedef struct
{
    /*
     * 节点唯一标识（例如 NODE01、NODE02）。
     * 作为注册表的查找主键。
     */
    char node_id[DEVICE_NODE_ID_SIZE];

    /*
     * 该设备通过哪条链路接入。
     * 决定下行命令应该写串口还是写 TCP 连接。
     */
    device_transport_t transport;

    /*
     * 与设备通信的 fd，用于下发命令。
     *
     * WiFi：该节点独占的 client socket，可直接写入；
     * 蓝牙：所有节点共用网关的串口 fd（写入即广播到总线）。
     */
    int fd;

    /*
     * 在线标记：1 在线，0 离线。
     *
     * 由 register/set_online 维护，
     * 与 last_seen 配合可判断设备是否已超时失联。
     */
    int online;

    /*
     * 服务该设备的 worker 线程标识。
     *
     * 注意：WiFi worker 以 detached 方式运行，无法 join。
     * 需要其停止时应依靠 running 标志或关闭 fd 让其自行退出，
     * 这里保存 tid 主要用于识别与诊断。
     */
    pthread_t thread_id;

    /*
     * 最近一次收到该设备数据的时间。
     *
     * 用于判断设备是否仍然活跃（配合在线超时阈值），
     * 也便于排查"注册了但早已不再上报"的僵尸节点。
     */
    time_t last_seen;

    /*
     *当前设备自己的下行命令队列
     *
     *Server Link Thread: 
     *     enqueue
     *
     * 对应设备Worker：
     *     dequeue
     */

    frame_command_t command_queue[
        DEVICE_COMMAND_QUEUE_CAPACITY
     ];

    size_t command_head;
    size_t command_tail;
    size_t command_count; 

} device_t;


/*
 * 设备注册表。
 *
 * 固定容量的哈希/线性表（见 .c 的实现），
 * 保存全部已接入设备并提供线程安全的查增删改。
 */
typedef struct
{
    /*
     * 设备条目数组，按注册顺序使用前 count 个。
     * 固定容量避免运行期分配，容量见 DEVICE_MANAGER_MAX_DEVICES。
     */
    device_t devices[DEVICE_MANAGER_MAX_DEVICES];

    /*
     * 当前已注册的设备数量（devices 中有效元素的个数）。
     */
    size_t count;

    /*
     * 保护 devices 与 count 的互斥锁。
     *
     * 所有接口（含 find 的复制过程）都必须持锁访问，
     * 因为多个 worker 线程会并发读写。
     */
    pthread_mutex_t mutex;

} device_manager_t;


/*
 * 初始化 Device Manager。
 *
 * 清零注册表并初始化内部 mutex。
 * 必须在其他接口被调用之前、由单一线程调用一次。
 *
 * @param manager 待初始化的管理器（调用前无需清零）
 *
 * @return DEVICE_MANAGER_OK；参数为 NULL 或 mutex 初始化失败时返回 ERROR
 */
int device_manager_init(
    device_manager_t *manager
);


/*
 * 注册一个新设备。
 *
 * 典型调用时机：
 *   - bluetooth_worker 首次从某 node_id 收到数据；
 *   - wifi_client_worker 接入并解析出 node_id 后。
 *
 * node_id 已存在时返回 DEVICE_MANAGER_EXISTS，
 * 调用方通常将其视为"节点重连"，改用 set_online 更新 fd。
 *
 * @param manager   管理器
 * @param node_id   节点标识（如 "NODE01"），不能为 NULL
 * @param transport 链路类型，决定下行命令的发送方式
 * @param fd        与该设备通信的 fd（WiFi 为 client socket，蓝牙为串口 fd）
 * @param thread_id 服务该设备的 worker 线程标识（主要用于识别与诊断）
 *
 * @return OK / EXISTS（重复注册）/ FULL（注册表已满）/ ERROR（参数非法）
 */
int device_manager_register(
    device_manager_t *manager,
    const char *node_id,
    device_transport_t transport,
    int fd,
    pthread_t thread_id
);


/*
 * 注销一个设备。
 *
 * 典型调用时机：WiFi 节点断开连接、worker 线程准备退出时。
 *
 * @param manager 管理器
 * @param node_id 要删除的节点标识
 *
 * @return OK / NOT_FOUND（该节点未注册）/ ERROR（参数非法）
 */
int device_manager_unregister(
    device_manager_t *manager,
    const char *node_id
);


/*
 * 查找设备。
 *
 * 注意：
 * 不返回 manager 内部指针，
 * 而是复制到 out_device。
 *
 * 这样做的原因：注册表内容会被其他线程随时修改，
 * 返回内部指针会让调用方在锁外访问到可能已失效的数据；
 * 复制一份快照则保证调用方拿到的是一致的数据。
 *
 * @param manager    管理器
 * @param node_id    要查找的节点标识
 * @param out_device 输出：设备信息副本（成功时填充）
 *
 * @return OK / NOT_FOUND / ERROR（参数非法）
 */
int device_manager_find(
    device_manager_t *manager,
    const char *node_id,
    device_t *out_device
);


/*
 * 修改设备在线状态和当前 fd。
 *
 * 用于两种情况：
 *   1. 节点重连后 fd 发生变化（旧连接已关闭）；
 *   2. 标记设备上线/离线（online 置 1 或 0）。
 *
 * @param manager 管理器
 * @param node_id 目标节点标识
 * @param online  新的在线状态：1 在线，0 离线
 * @param fd      新的通信 fd（传 -1 表示当前无可用连接）
 *
 * @return OK / NOT_FOUND / ERROR（参数非法）
 */
int device_manager_set_online(
    device_manager_t *manager,
    const char *node_id,
    int online,
    int fd
);


/*
 * 更新设备最后活动时间。
 *
 * 每次收到该设备的上行数据时调用，
 * 把 last_seen 刷新为当前时间。
 *
 * 注意：建议上层做节流（例如最快每秒更新一次），
 * 否则高频数据会让每次上报都产生一次加锁开销。
 *
 * @param manager 管理器
 * @param node_id 目标节点标识
 *
 * @return OK / NOT_FOUND / ERROR（参数非法）
 */
int device_manager_update_last_seen(
    device_manager_t *manager,
    const char *node_id
);


/*
 * 销毁 Device Manager。
 *
 * 调用前必须确保其他线程已经停止访问 manager。
 *
 * 典型调用位置：gateway_loop_run() 的退出清理段，
 * 且必须排在所有 worker 线程 join / 等待完成之后，
 * 否则会销毁一个仍被使用的 mutex（未定义行为）。
 *
 * @param manager 管理器
 */
void device_manager_destroy(
    device_manager_t *manager
);

/*
 * 将一个物理连接绑定到 node_id。
 *
 * node_id 不存在：
 *     新建设备。
 *
 * node_id 已存在且 transport 一致：
 *     认为是设备重连，更新 fd/thread_id/online。
 */
int device_manager_bind_connection(
    device_manager_t *manager,
    const char *node_id,
    device_transport_t transport,
    int fd,
    pthread_t thread_id
);


/*
 * 当前连接断开。
 *
 * 只有 Device Manager 中保存的
 * fd + thread_id 仍然属于这个调用者时，
 * 才允许把设备标记为 offline。
 *
 * 防止旧 Worker 误伤新的重连。
 */
int device_manager_disconnect(
    device_manager_t *manager,
    const char *node_id,
    int fd,
    pthread_t thread_id
);

/*
 * 向指定在线节点投递一条命令。
 *
 * 只做内存复制，不做任何socket/serial I/O。
 */
int device_manager_enqueue_command(
    device_manager_t *manager,
    const char *node_id,
    const frame_command_t *command
);


/*
 * 当前设备Worker尝试取得一条自己的命令。
 *
 * fd + thread_id 用于确认调用者仍然是
 * Device Manager 当前记录的连接拥有者。
 */
int device_manager_try_dequeue_command(
    device_manager_t *manager,
    const char *node_id,
    int fd,
    pthread_t thread_id,
    frame_command_t *out_command
);

int device_manager_disconnect_collect_commands(
    device_manager_t *manager,
    const char *node_id,
    int fd,
    pthread_t thread_id,
    frame_command_t *out_commands,
    size_t command_capacity,
    size_t *out_count
);

int device_manager_bind_connection_collect_commands(
    device_manager_t *manager,
    const char *node_id,
    device_transport_t transport,
    int fd,
    pthread_t thread_id,
    frame_command_t *out_commands,
    size_t command_capacity,
    size_t *out_count
);

#endif