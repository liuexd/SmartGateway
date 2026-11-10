#ifndef SERVICE_IPC_H
#define SERVICE_IPC_H

#include <stddef.h>
#include <sys/types.h>

/*
 * service_ipc - Service Supervisor 的请求/响应协议（S5）
 *
 * 为什么需要这一层
 * ----------------
 * S3/S4 暴露了两个无法在 OTA 侧彻底解决的问题：
 *
 *   L3：前置校验依赖 pidfile 存在。若 Gateway 由别的方式启动
 *       （例如手工执行、或旧的 init 脚本），pidfile 不存在，
 *       OTA 会判定 STOPPED 并直接启动第二个实例。
 *
 *   L6：停止外部进程存在 TOCTOU 窗口。OTA 读到 (pid, starttime)
 *       之后、发信号之前，进程可能退出且 pid 被复用。pidfd
 *       需要 Linux 5.1+，目标板是 4.9.88，无法消除。
 *
 * 两者的根因相同：**OTA 不拥有那个进程**。它只能靠 pidfile 猜测，
 * 靠 pid 去发信号。
 *
 * 解决办法是把所有权交给一个常驻的 supervisor：
 *   - 由它 fork/exec Gateway，因此它是父进程，waitpid 总是有效
 *   - 由它维护 pidfile，永远与真实状态一致
 *   - OTA 不再 kill 任何进程，而是请求 supervisor 执行
 *
 * 这样 L6 的 TOCTOU 就消失了：supervisor 停止的是自己的子进程，
 * 不存在"pid 被复用"的可能（未被回收的子进程 pid 不会被复用）。
 *
 * 协议选择：Unix Domain Socket + 单行文本
 * ----------------------------------------
 * 不用 JSON：请求只有两三个字段，文本单行已经足够，且
 *   - 出错时可以用 telnet/nc 手工调试（板端没有 json 工具）
 *   - 解析代码极短，减少攻击面
 *   - 不依赖 json-c（supervisor 应尽可能少依赖）
 *
 * 不用 TCP：不需要跨机器，Unix socket 有文件权限保护。
 *
 * 安全约束：socket 只允许 root 访问（0600）。
 * 原因：能停止服务的人等于能中断业务，不该给普通用户。
 */

#define SERVICE_IPC_SOCKET_NAME "supervisor.sock"

/*
 * socket 路径：<run_dir>/supervisor.sock
 *
 * run_dir 通常是 /opt/smartgateway/run。
 *
 * 为什么放在文件系统而不是抽象命名空间：
 *   抽象命名空间（sun_path[0] == '\0'）无法用文件权限限制访问，
 *   且重启后不会残留——看起来方便，实际上很难排查
 *   "为什么连不上"。用真实路径可以看到 socket 文件在不在、
 *   权限对不对。
 */
int service_ipc_path(
    const char *run_dir,
    char *output,
    size_t capacity);

/* ------------------------------------------------------------------ */
/* 请求                                                                */
/* ------------------------------------------------------------------ */

/*
 * 支持的请求。
 *
 * 刻意保持极小：supervisor 的职责是"管进程"，不是"管业务"。
 * 任何与 OTA 状态、版本相关的判断都必须留在 OTA 侧——
 * 让 supervisor 参与 OTA 决策会制造第二个真相源。
 */
typedef enum {
    SERVICE_IPC_UNKNOWN = 0,

    /* 返回服务当前状态 */
    SERVICE_IPC_STATUS,

    /* 启动服务（已在运行则返回 ALREADY） */
    SERVICE_IPC_START,

    /* 停止服务并等待确认退出 */
    SERVICE_IPC_STOP,

    /* 重载配置（重新读取配置文件，不重启进程） */
    SERVICE_IPC_RELOAD
} service_ipc_request_t;

/*
 * 一条请求。
 *
 * 绝大多数请求不需要参数，但 START 需要一种例外：OTA 的健康检查
 * 必须让 Gateway 连到一个**测试用**的上游地址（Mock Server），
 * 而正常运行应使用配置文件里的地址。
 *
 * 为什么允许覆盖而不是让 OTA 自己启动
 * ----------------------------------
 * 让 OTA 自己启动就回到了"OTA 拥有进程"的老路，L3/L6 立刻复现。
 * 允许 START 携带一次性覆盖参数，既保持 supervisor 的所有权，
 * 又能满足健康检查的需要。
 *
 * 覆盖只对本次启动有效，不写入配置文件——否则一次健康检查
 * 会把生产配置永久改成测试地址，这是极其危险的副作用。
 */
typedef struct {
    service_ipc_request_t request;

    /*
     * 仅 START 使用。为 0/空表示"用配置文件的值"。
     *
     * upstream_port 是 Gateway **连接**的北向端口，
     * 与 service.conf 里的 port 语义一致。
     */
    int upstream_port;
    char upstream_server[64];
} service_ipc_call_t;

/*
 * 响应状态码。
 *
 * 用固定字符串而非数字：日志里一眼能看懂，
 * 且不会出现"3 是什么来着"的问题。
 */
typedef enum {
    SERVICE_IPC_OK = 0,        /* 成功 */
    SERVICE_IPC_ALREADY,       /* 已在目标状态（幂等） */
    SERVICE_IPC_NOT_RUNNING,   /* 当前未运行 */
    SERVICE_IPC_BUSY,          /* 正在处理另一个请求 */
    SERVICE_IPC_FAILED,        /* 操作失败 */
    SERVICE_IPC_BAD_REQUEST    /* 请求格式错误 */
} service_ipc_result_t;

const char *service_ipc_result_name(service_ipc_result_t result);
int service_ipc_result_parse(
    const char *name, service_ipc_result_t *out);

/*
 * 一条响应。
 *
 * pid/starttime 用于让调用方判断"还是不是当初那个进程"，
 * 与 OTA 的 pidfile 语义一致，便于两侧对齐。
 * 未运行时为 0。
 */
#define SERVICE_IPC_MESSAGE_MAX 256

typedef struct {
    service_ipc_result_t result;
    pid_t pid;
    unsigned long long starttime;
    char detail[SERVICE_IPC_MESSAGE_MAX];
} service_ipc_reply_t;

/* ------------------------------------------------------------------ */
/* 编解码                                                              */
/* ------------------------------------------------------------------ */

/*
 * 编码请求为单行文本（不含换行）。
 *
 * 返回写出的字节数；-1 表示缓冲区不足。
 */
int service_ipc_encode_request(
    service_ipc_request_t request,
    char *output,
    size_t capacity);

/*
 * 编码带参数的请求。
 *
 * START 可选携带覆盖参数：
 *   START <port> <server>
 * 其余请求忽略 call 中的参数字段。
 *
 * 注意：端口与服务器都必须是显式合法的值。
 * 端口 0 或非数字、服务器含空白 → 返回 -1。
 * 不接受"部分参数"——要么都给出，要么都不给，
 * 避免解析时出现含糊的中间态。
 */
int service_ipc_encode_call(
    const service_ipc_call_t *call,
    char *output,
    size_t capacity);

/*
 * 解析单行文本请求。允许尾部空白与 \r\n。
 *
 * call_out 中的 request 字段被填充；START 若携带参数，
 * 也一并解析到 upstream_port/upstream_server。
 */
int service_ipc_decode_request(
    const char *line,
    service_ipc_request_t *out);

int service_ipc_decode_call(
    const char *line,
    service_ipc_call_t *out);

/*
 * 编码响应。
 *
 * 格式：RESULT <pid> <starttime> <detail...>
 * detail 中的换行会被替换为空格，保证单行。
 */
int service_ipc_encode_reply(
    const service_ipc_reply_t *reply,
    char *output,
    size_t capacity);

/*
 * 解析响应。
 *
 * 严格校验：字段缺失、数字溢出、result 未知一律返回 -1。
 * 不做"尽量猜"的处理——一个看不懂的响应应当被视为通信故障，
 * 而不是某个默认状态。
 */
int service_ipc_decode_reply(
    const char *line,
    service_ipc_reply_t *out);

/* ------------------------------------------------------------------ */
/* 客户端                                                              */
/* ------------------------------------------------------------------ */

/*
 * 向 supervisor 发送一条请求并等待响应。
 *
 * timeout_ms 覆盖整个"连接 + 发送 + 接收"过程。
 *
 * 返回 0 表示通信成功（reply_out 已填充，具体结果看 reply->result）；
 * 返回 -1 表示通信失败（errno 说明原因：ENOENT 表示 supervisor
 * 未运行，ECONNREFUSED 表示 socket 存在但无人监听）。
 *
 * 区分这两种失败很重要：ENOENT 是"没装服务"，
 * ECONNREFUSED 是"装了但挂了"，处置方式不同。
 */
int service_ipc_call(
    const char *run_dir,
    service_ipc_request_t request,
    int timeout_ms,
    service_ipc_reply_t *reply_out);

/*
 * 带参数的调用（START 使用）。
 */
int service_ipc_call_ex(
    const char *run_dir,
    const service_ipc_call_t *call,
    int timeout_ms,
    service_ipc_reply_t *reply_out);

/* ------------------------------------------------------------------ */
/* 服务端                                                              */
/* ------------------------------------------------------------------ */

/*
 * 创建并绑定监听 socket。
 *
 * 返回 listen fd；-1 失败。
 * 会先检查是否已有实例在监听：若有，返回 -1 且 errno == EADDRINUSE，
 * 调用方据此避免启动第二个 supervisor。
 *
 * socket 文件权限设为 0600，只有 root 能访问。
 */
int service_ipc_listen(
    const char *run_dir,
    char *path_out,
    size_t path_capacity);

/*
 * 接受一个连接并读取完整的一行请求。
 *
 * 返回 >=0 为已连接的 fd（调用方负责 close），-1 表示失败。
 * 若 errno == EAGAIN 表示暂无连接（非阻塞场景）。
 *
 * 超时由调用方通过 poll 控制；本函数只处理单次 accept + read。
 */
int service_ipc_accept_request(
    int listen_fd,
    service_ipc_request_t *request_out);

/*
 * 同 service_ipc_accept_request，但保留 START 携带的参数。
 */
int service_ipc_accept_call(
    int listen_fd,
    service_ipc_call_t *call_out);

/* 向客户端回写响应并关闭连接。 */
int service_ipc_send_reply_and_close(
    int client_fd,
    const service_ipc_reply_t *reply);

#endif /* SERVICE_IPC_H */
