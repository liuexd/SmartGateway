#ifndef OTA_OPLOG_H
#define OTA_OPLOG_H

#include <limits.h>
#include <sys/types.h>

/*
 * ota_oplog - 未完成操作的旁路记录（S4/F2）
 *
 * 为什么需要它
 * ------------
 * ota_state.json 是**权威状态**，但它只在状态机转换时更新。
 * 而升级过程中存在若干"已经产生副作用、但还没到下一次状态
 * 转换"的时刻：
 *
 *   停掉旧版 Gateway  ->  ...  -> 切换 current  ->  启动新版
 *                      ^
 *                      |  如果 OTA 进程在这里被 kill -9 或掉电，
 *                      |  没有任何记录能说明"旧版是被 OTA 停的"
 *
 * 结果是下次开机：state 仍是 CONFIRMED、current 仍指向旧版，
 * 一切看起来正常——但业务其实已经停了，而没有任何一方知道
 * 曾经发生过一次未完成的升级。
 *
 * last_operation.json 补的就是这个空档：它记录"当前正在做什么"，
 * 正常结束时清除。启动时若发现它还在，说明上一次没有善终。
 *
 * 它与 ota_state.json 的职责边界（必须遵守）
 * ------------------------------------------
 *   权威状态：  ota_state.json + current 软链
 *   辅助信息：  last_operation.json
 *
 * 因此：
 *   - 恢复逻辑**不得**依赖 last_operation 做出"该回滚还是该继续"
 *     的决定。文件系统一致性必须由 state + current 独立验证。
 *   - last_operation 只用于"检测到未完成时告警并拒绝自动继续"，
 *     即把"静默的错误状态"变成"显式的人工决策点"。
 *
 * 设计取舍：宁可阻塞，不可静默继续
 * --------------------------------
 * 检测到未完成记录时，本模块的策略是**一律拒绝继续升级**，
 * 而不是自动清理。理由：
 *   自动清理等价于宣称"上次的残留无害"，而我们无法证明这一点——
 *   残留可能来自"旧版已停、新版未起"这种业务中断状态。
 *   让 OTA 自己做这个判断，等于让它替运维签字。
 *
 * 人工确认路径：ota_agent ack-operation <install_dir>
 */

/*
 * 记录中的 phase 取值。
 *
 * 这些名字是**给人看的**，不是状态机。不要试图把它们与
 * ota_state_t 建立一一映射——两者语义不同：
 * state 表示"版本处于什么生命周期阶段"，
 * phase 表示"上一次操作走到了哪一步"。
 */
typedef enum {
    OTA_OPLOG_NONE = 0,       /* 无记录 */
    OTA_OPLOG_STOPPING_OLD,   /* 正在停止旧版 Gateway */
    OTA_OPLOG_SWITCHING,      /* 正在切换 current */
    OTA_OPLOG_STARTING_NEW,   /* 正在启动新版 */
    OTA_OPLOG_HEALTH_CHECK,   /* 正在做 L1/L2 健康检查 */
    OTA_OPLOG_ROLLING_BACK,   /* 正在回滚 */
    OTA_OPLOG_DONE,           /* 已完成（正常终态） */
    OTA_OPLOG_FAILED          /* 已失败并处理完毕（正常终态） */
} ota_oplog_phase_t;

/*
 * 一条操作记录的完整内容。
 *
 * pid 与 boot_id 不是必需的，只是帮助判断"这条记录是不是
 * 同一次开机留下的"。它们不参与任何自动决策。
 */
typedef struct {
    ota_oplog_phase_t phase;
    pid_t pid;
    long long started_at;     /* Unix 时间戳（秒） */
    char boot_id[40];
} ota_oplog_record_t;

/* phase <-> 字符串 */
const char *ota_oplog_phase_name(ota_oplog_phase_t phase);
int ota_oplog_phase_parse(
    const char *name, ota_oplog_phase_t *out);

/*
 * 判断某 phase 是否表示"操作已正常收尾"。
 *
 * 只有 DONE 与 FAILED 算收尾：它们意味着操作要么成功、
 * 要么失败但已经把状态处理干净。其余都表示"可能中断"。
 */
int ota_oplog_phase_is_terminal(ota_oplog_phase_t phase);

/* <install_dir>/last_operation.json */
int ota_oplog_path(
    const char *install_dir,
    char *output,
    size_t capacity);

/*
 * 写入一条记录（原子替换，带 fsync）。
 *
 * 失败时返回 -1 并设置 errno。调用方可以选择忽略，
 * 但本项目要求：写不进去就**不能**继续升级——
 * 因为无法留下"我正在操作"的证据，就等于放弃了中断检测能力。
 */
int ota_oplog_write(
    const char *install_dir,
    const ota_oplog_record_t *record);

/*
 * 读取记录。
 *
 * 返回：
 *    0  读到合法记录，*present 置 1
 *    0  无记录（文件不存在），*present 置 0
 *   -1  记录存在但无法解析（视为损坏，调用方应按"未完成"处理）
 *
 * 注意"文件存在但损坏"不能当作"没有记录"：损坏本身
 * 就说明上次写入被打断了，同样属于未完成的情形。
 */
int ota_oplog_read(
    const char *install_dir,
    ota_oplog_record_t *record_out,
    int *present);

/* 删除记录。返回 0 成功或本来就不存在；-1 真实失败。 */
int ota_oplog_remove(const char *install_dir);

/*
 * 便捷封装：开始一个阶段。
 *
 * 自动填充 pid、started_at、boot_id，调用方只需给出 phase。
 */
int ota_oplog_begin(
    const char *install_dir,
    ota_oplog_phase_t phase);

#endif /* OTA_OPLOG_H */
