#ifndef SERVICE_SUPERVISOR_H
#define SERVICE_SUPERVISOR_H

#include <limits.h>
#include <sys/types.h>

/*
 * service_supervisor - Gateway 进程的唯一所有者（S5）
 *
 * 它解决什么
 * ----------
 * L3：OTA 的前置校验依赖 pidfile 存在。若 Gateway 由别的方式启动，
 *     pidfile 不存在，OTA 会判定 STOPPED 并启动第二个实例。
 *
 * L6：OTA 停止"外部进程"存在 TOCTOU 窗口——读到 (pid, starttime)
 *     之后、发信号之前，进程可能退出且 pid 被复用。pidfd 需要
 *     Linux 5.1+，目标板是 4.9.88，无法消除。
 *
 * 共同根因：**OTA 不拥有那个进程**。
 *
 * Supervisor 是 Gateway 的父进程，因此：
 *   - waitpid 总是有效，能确认"我启动的那个"确实退出了
 *   - pidfile 由它维护，永远与真实状态一致
 *   - 停止动作作用于自己的子进程，不存在 pid 被复用的可能
 *     （未回收的子进程 pid 不会被内核复用）
 *
 * 职责边界（必须守住）
 * -------------------
 * Supervisor **只管进程**，不管业务：
 *   - 不读 OTA 状态、不判断版本、不碰 releases 目录
 *   - 不参与"该不该升级"的决策
 *
 * 一旦让它参与 OTA 决策，就会出现第二个真相源，
 * 而两个真相源迟早会不一致。
 *
 * 崩溃自愈的取舍
 * --------------
 * Supervisor **不**自动重启 Gateway。
 *
 * 理由：OTA 升级期间需要 Gateway 保持停止状态（先在旧版退出、
 * 后在新版上场）。若 supervisor 自动拉起，会在升级中途把旧版
 * 拉起来，与"切换 current"形成竞争。
 *
 * 崩溃自愈改由 init 的 respawn 机制负责，见 S5 的 init 脚本。
 * 两者分工：
 *   - Supervisor：确定性地启动/停止/查询（OTA 需要）
 *   - init respawn：监督 supervisor 本身，supervisor 挂了才重启
 */

/* 运行目录内的固定文件名 */
#define SERVICE_PIDFILE_NAME "service.pid"
#define SERVICE_CONFIG_NAME  "service.conf"

/*
 * 服务配置。
 *
 * 从 <run_dir>/../service.conf 读取，避免把启动参数硬编码进二进制。
 * 板端实际路径：/opt/smartgateway/service.conf
 */
typedef struct {
    char executable[PATH_MAX];   /* Gateway 绝对路径 */
    char serial[PATH_MAX];       /* 串口设备；空表示 --no-serial */
    int  baud;                   /* 波特率，仅串口模式使用 */
    char server[64];             /* 北向服务器 IP */
    int  port;                   /* 北向服务器端口 */

    /* 停止超时：SIGTERM 等待时长，超时后 SIGKILL */
    int  stop_timeout_ms;
} service_config_t;

/*
 * 读取配置。
 *
 * 支持缺省：配置文件中未出现的键使用内置默认值。
 * 但 executable 必须显式给出——猜一个可执行文件路径去运行是危险的。
 *
 * 返回 0 成功；-1 失败（errno 说明原因）。
 */
int service_config_load(
    const char *config_path,
    service_config_t *config_out);

/*
 * 写一份默认配置（仅用于首次部署，不会覆盖已存在的文件）。
 */
int service_config_write_default(
    const char *config_path,
    const char *executable);

/*
 * 运行中的服务状态。
 */
typedef struct {
    int running;
    pid_t pid;
    unsigned long long starttime;
} service_state_t;

/*
 * 查询服务状态。
 *
 * 判定依据是 pidfile 中的 (pid, starttime) 与 /proc 实时值比对，
 * 与 OTA 的 ota_sup_verify_identity 采用同一套语义。
 *
 * 注意：pidfile 由 supervisor 自己写，所以它的可信度高于
 * OTA 场景下的外部 pidfile——但比对逻辑仍必须完整，
 * 因为 supervisor 崩溃后 pidfile 会残留。
 */
int service_state_query(
    const char *run_dir,
    service_state_t *state_out);

/*
 * Supervisor 主循环。
 *
 * 参数：
 *   run_dir      运行目录（socket 与 pidfile 所在）
 *   config_path  配置文件路径
 *   foreground   1 表示不 daemonize（便于 init/调试）
 *
 * 返回 0 表示正常退出（收到 SIGTERM/SIGINT）；-1 表示启动失败。
 *
 * 阻塞运行，直到收到终止信号。
 */
int service_supervisor_run(
    const char *run_dir,
    const char *config_path,
    int foreground);

#endif /* SERVICE_SUPERVISOR_H */
