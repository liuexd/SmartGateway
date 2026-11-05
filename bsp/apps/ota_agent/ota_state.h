
#ifndef OTA_STATE_H
#define OTA_STATE_H

/*
 * OTA 状态机的全部状态。
 * 正常升级链: IDLE -> DOWNLOADED -> VERIFIED -> STAGED
 *             -> ACTIVATED -> HEALTHY -> CONFIRMED
 * 回滚分支  : STAGED / ACTIVATED / HEALTHY -> ROLLBACK_PENDING -> ROLLED_BACK
 * 终态      : CONFIRMED（新版本已被确认，不再回滚）
 *             ROLLED_BACK（已回滚到旧版本）
 * OTA_STATE_COUNT 不是真实状态，只用来做上界检查和名字表长度（哨兵值）。
 */
typedef enum {
    OTA_IDLE = 0,
    OTA_DOWNLOADED,
    OTA_VERIFIED,
    OTA_STAGED,
    OTA_ACTIVATED,
    OTA_HEALTHY,
    OTA_CONFIRMED,
    OTA_ROLLBACK_PENDING,
    OTA_ROLLED_BACK,
    OTA_STATE_COUNT
} ota_state_t;

/*
 * ota_context_t - 状态机的"运行时上下文"
 *
 * 作用: 把状态机的可变状态打包成一个对象，作为
 *       ota_state_transition(ctx, next) 的操作对象。
 *       该函数需要"读当前状态 -> 校验是否允许 -> 写入新状态"这一读改写过程，
 *       所以必须按指针传入，不能按值传。
 *
 * 为什么不用裸的 ota_state_t *:
 *   1) 可扩展: 后续需要加入重试次数、当前启动槽位、健康检查结果等字段时，
 *      只需往结构体里加成员，函数签名和所有调用点都不必改动；
 *      若直接把 ota_state_t * 当接口，改动将波及整个代码库。
 *   2) 语义区分: ota_state_can_transition(from, to) 里状态是只读的、按值传；
 *      ota_state_transition(ctx, next) 里上下文是可变的、按指针传，
 *      两种用法一眼可辨。
 *   3) 可快照: 需要时可以直接复制一份（如 ota_context_t saved = ctx;），
 *      转换失败后用于回退。
 *
 * 注意: 目前只有一个字段，sizeof 与 ota_state_t 相同，没有额外开销；
 *       它与 ota_state_record_t（ota_state_io.h 中定义）不同:
 *         ota_context_t     只在内存中参与状态机运算，用完即弃；
 *         ota_state_record_t 要落盘，除 state 外还带两个版本号字符串。
 */
typedef struct {
    ota_state_t state;
} ota_context_t;

/* 将状态枚举转换成字符串 */
const char *ota_state_name(ota_state_t state);

/* 将状态字符串转换成枚举 */
int ota_state_parse(
    const char *name,
    ota_state_t *out_state
);

/* 判断是否允许状态转换 */
int ota_state_can_transition(
    ota_state_t from,
    ota_state_t to
);

/* 执行状态转换，成功返回0，失败返回-1 */
int ota_state_transition(
    ota_context_t *ctx,
    ota_state_t next
);

#endif
