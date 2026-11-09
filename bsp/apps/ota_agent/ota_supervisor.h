#ifndef OTA_SUPERVISOR_H
#define OTA_SUPERVISOR_H

#include <sys/types.h>

/*
 * OTA Supervisor —— 统一的门禁进程管理接口（D25-2 S1）
 *
 * 设计边界（务必先读）：
 *
 *   1. 本模块**只做"记录、查询、身份校验"**，不负责启动或杀死进程。
 *      启动/停止仍由 ota_process.c 与未来的 init 脚本负责。
 *      之所以拆开，是因为"进程归谁所有"决定了能否使用 waitpid：
 *        - OTA 自己 fork 的子进程   -> 用 ota_process_stop()
 *        - init 启动的常驻进程      -> 只能发信号 + 由 init 回收
 *      把这两件事混在一个接口里，必然出现"误杀"或"回收失败"。
 *
 *   2. 本模块**绝不依据进程名匹配**。按名字找进程会误杀同名进程，
 *      这是生产环境的经典事故。身份只由 (pid, starttime) 二元组确定。
 *
 *   3. 查询是**纯只读**的：不发信号、不回收僵尸、不修改任何文件。
 *      任何带副作用的动作都不能塞进 query，否则"查询"会改变系统状态。
 *
 *   状态语义：
 *     RUNNING  身份校验通过，确认是当初记录的那个进程实例
 *     STOPPED  记录不存在，或进程已不存在，或 pid 已被复用
 *     UNKNOWN  无法判定（pidfile 损坏、/proc 不可读、pid<=1 等）
 *
 *   重要约定：调用方遇到 UNKNOWN **必须中止升级**，不能猜测。
 *   "状态不确定时继续操作"是这类系统最危险的失效模式。
 */

typedef enum {
    OTA_SUP_STOPPED = 0,
    OTA_SUP_RUNNING,
    OTA_SUP_UNKNOWN
} ota_sup_state_t;

/*
 * pidfile 中记录的身份。
 *
 * 为什么需要 starttime：
 *   pid 会被内核复用。旧 pidfile 里的 pid 可能已被无关进程占用，
 *   此时"进程存在"是假阳性，按它发信号就会误杀。
 *   starttime 取自 /proc/<pid>/stat 第 22 字段（进程启动时刻，单位 jiffies），
 *   pid 复用后该值必然不同，因此 (pid, starttime) 能唯一确定一个实例。
 *
 * 注意：starttime 单独**不能**作唯一标识——同一时钟节拍内 fork 的多个
 * 进程 starttime 相同（已实测）。唯一性来自 pid，starttime 只用于
 * 检测"这个 pid 还是不是当初那个进程"。
 */
typedef struct {
    pid_t pid;
    unsigned long long starttime;
} ota_sup_identity_t;

/*
 * 生成 pidfile 用的规范路径。
 *
 * 路径为 <install_dir>/run/gateway.pid。
 * output 建议至少 PATH_MAX 字节。
 *
 * 返回 0 成功；-1 失败（参数非法或路径被截断）。
 */
int ota_sup_pidfile_path(
    const char *install_dir,
    char *output,
    size_t capacity);

/*
 * 读取 /proc/<pid>/stat 的 starttime。
 *
 * 正确实现必须从**最后一个** ')' 之后解析：
 *   comm 字段允许包含空格和括号，按空白分列会静默取到错误值。
 *   （实测：comm="a b (c) )" 时 awk $22 返回 0，实际值数千万）
 *
 * 返回 0 成功；-1 失败，errno 有效（ENOENT 表示进程不存在）。
 */
int ota_sup_read_starttime(
    pid_t pid,
    unsigned long long *starttime_out);

/*
 * 采集当前进程的身份（pid + starttime）。
 *
 * 用于"我刚启动了一个进程，把它登记下来"的场景。
 *
 * 返回 0 成功；-1 失败。
 */
int ota_sup_capture_identity(
    pid_t pid,
    ota_sup_identity_t *identity_out);

/*
 * 原子写入 pidfile。
 *
 * 实现要求：
 *   - 写临时文件 -> fsync -> rename -> fsync 目录
 *   - 临时文件与目标在同一目录，保证 rename 原子
 *   - 权限 0600，不跟随符号链接
 *
 * 这样即使写入过程中掉电，也不会留下半截 pidfile：
 * 要么是旧内容，要么是完整新内容。
 *
 * 返回 0 成功；-1 失败，errno 有效。
 */
int ota_sup_pidfile_write(
    const char *install_dir,
    const ota_sup_identity_t *identity);

/*
 * 读取 pidfile。
 *
 * 严格校验内容格式：必须恰好是一行十进制 pid + 一个十进制 starttime。
 * 任何多余字符、空行、超长内容都视为损坏。
 *
 * 返回：
 *    0  读取成功
 *   -1  失败，errno 有效
 *        ENOENT  文件不存在（正常情况：还没启动过）
 *        EINVAL  内容损坏或格式非法
 *        ELOOP   是符号链接（拒绝跟随）
 */
int ota_sup_pidfile_read(
    const char *install_dir,
    ota_sup_identity_t *identity_out);

/*
 * 删除 pidfile（忽略不存在的情况）。
 *
 * 返回 0 成功或文件本就不存在；-1 失败。
 */
int ota_sup_pidfile_remove(const char *install_dir);

/*
 * 校验身份对应的进程是否仍是当初那个实例。
 *
 * 纯只读，不发信号。
 *
 * 返回：
 *   OTA_SUP_RUNNING  存在且 starttime 一致
 *   OTA_SUP_STOPPED  /proc/<pid> 不存在，或 starttime 不一致（pid 已复用）
 *   OTA_SUP_UNKNOWN  无法判定（pid<=1、stat 读取失败但非 ENOENT、格式异常）
 *
 * 注意 pid<=1 永远返回 UNKNOWN：绝不能对 init 发信号。
 */
ota_sup_state_t ota_sup_verify_identity(
    const ota_sup_identity_t *identity);

/*
 * 一步查询 Gateway 当前状态（读 pidfile + 校验身份）。
 *
 * pid_out 可为 NULL；非空时在返回 RUNNING 时写入 pid。
 *
 * 返回：
 *   OTA_SUP_STOPPED  pidfile 不存在、内容损坏但进程显然不在、或 pid 已复用
 *   OTA_SUP_RUNNING  身份校验通过
 *   OTA_SUP_UNKNOWN  无法判定
 *
 * 区分说明：
 *   pidfile 不存在            -> STOPPED（从未启动或已正常清理）
 *   pidfile 存在但内容损坏    -> UNKNOWN（可能真有问题，也可能被外部破坏，
 *                                两者都不能当作"安全可升级"）
 *   符号链接、权限错误等      -> UNKNOWN
 */
ota_sup_state_t ota_sup_query(
    const char *install_dir,
    pid_t *pid_out);

/*
 * 把状态枚举转成可读字符串，用于日志与测试输出。
 */
const char *ota_sup_state_name(ota_sup_state_t state);

#endif
