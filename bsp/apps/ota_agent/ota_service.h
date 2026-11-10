#ifndef OTA_SERVICE_H
#define OTA_SERVICE_H

#include <sys/types.h>

/*
 * ota_service - OTA 侧的 Gateway 服务客户端（S5）
 *
 * 设计决定：强制走 IPC
 * --------------------
 * S5 之前 OTA 自己维护 pidfile（<install>/run/gateway.pid），
 * 靠自己读 /proc 判断状态、靠自己 kill 停止进程。这留下两个
 * 无法在 OTA 侧彻底解决的问题：
 *
 *   L3  若 Gateway 由别的方式启动（手工执行、旧 init 脚本），
 *       pidfile 不存在，OTA 判定 STOPPED 并启动第二个实例。
 *
 *   L6  OTA 读到 (pid, starttime) 之后、发信号之前，进程可能
 *       退出且 pid 被复用。pidfd 需要 Linux 5.1+，板端是
 *       4.9.88，无法消除这个窗口。
 *
 * S5 引入常驻 supervisor 后，这两个问题的根因（OTA 不拥有那个
 * 进程）消失了：
 *   - supervisor 是 Gateway 的父进程，waitpid 总是有效
 *   - pidfile 由 supervisor 维护，永远与真实状态一致
 *   - 停止动作作用于自己的子进程，pid 不可能被复用
 *
 * 因此**本模块不做回退**：supervisor 不可用就拒绝升级。
 *
 * 为什么不保留"pidfile 回退路径"
 * ------------------------------
 * 看似更安全的"两条路"实际上是更危险的：
 *   回退路径会在 supervisor 挂掉时悄悄启用，而它正是那个有
 *   L3/L6 缺陷的实现。运维看到"升级成功"，却不知道自己走的
 *   是已知有缺陷的路径。
 *
 * 强制 IPC 的代价是"supervisor 挂了就不能升级"。这是**正确的
 * 失败方向**：拒绝升级不会破坏任何东西，而走缺陷路径可能
 * 杀掉无关进程。
 */

#include "service_ipc.h"

/* Gateway 服务当前状态 */
typedef enum {
    OTA_SERVICE_STOPPED = 0,
    OTA_SERVICE_RUNNING,
    OTA_SERVICE_UNKNOWN      /* 无法判定（通信失败或响应异常） */
} ota_service_state_t;

const char *ota_service_state_name(ota_service_state_t state);

/*
 * 查询 supervisor 是否可服务。
 *
 * 返回 0 表示可用（supervisor 在监听且能正常响应）；
 * -1 表示不可用，errno 说明原因：
 *     ENOENT       socket 不存在 -> supervisor 未运行
 *     ECONNREFUSED socket 在但无人监听 -> supervisor 崩溃
 *     ETIMEDOUT    supervisor 忙或卡死
 *     EINVAL       响应无法解析（协议不匹配，可能版本不一致）
 *
 * 升级开始时**必须**先调用本函数。不可用就中止，
 * 且要在产生任何副作用之前中止。
 */
int ota_service_probe(const char *install_dir);

/*
 * 查询 Gateway 状态。
 *
 * status_out 可为 NULL。running 时输出 pid 与 starttime，
 * 便于日志记录与事后核对（但不用于安全判断——
 * 安全判断由 supervisor 自己保证）。
 */
int ota_service_query(
    const char *install_dir,
    ota_service_state_t *state_out,
    pid_t *pid_out);

/*
 * 请求启动 Gateway。
 *
 * upstream_port/upstream_server 非空时覆盖本次启动的北向目标，
 * 用于健康检查（指向 Mock Server）。与 0/空表示用配置文件的值。
 *
 * 覆盖不会写回配置文件——那会把一次健康检查永久改掉生产配置。
 *
 * 返回：
 *    0                       已启动
 *    OTA_SERVICE_ERR_ALREADY 已在运行（幂等，非错误）
 *   -1                      失败
 *
 * 已运行的判定由 supervisor 给出，OTA 不自己猜。
 */
#define OTA_SERVICE_ERR_ALREADY 1

int ota_service_start(
    const char *install_dir,
    const char *upstream_server,
    int upstream_port,
    pid_t *pid_out);

/*
 * 请求停止 Gateway 并等待 supervisor 确认退出。
 *
 * 关键差异（与 S3/S4 的 ota_sup_stop_managed 相比）：
 *   supervisor 停的是**自己的子进程**，因此：
 *   - 不需要 OTA 再读 /proc 校验身份
 *   - 不存在 pid 复用误杀的可能
 *   - 返回 OK 就意味着进程确实已经退出
 *
 * 返回：
 *    0                         已停止
 *    OTA_SERVICE_ERR_NOT_RUNNING 本来就没运行（幂等，非错误）
 *   -1                        失败
 */
#define OTA_SERVICE_ERR_NOT_RUNNING 2

int ota_service_stop(const char *install_dir);

#endif /* OTA_SERVICE_H */
