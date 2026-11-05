
#define _POSIX_C_SOURCE 200809L

#include "ota_state_io.h"
#include "ota_verify.h"
#include "ota_recover.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RECOVER_TEMP_LINK "current.recover.next"

/* 兼容 D19 的 v 前缀和 D20 的规范版本号 */
static int build_target(
    const char *version,
    char *output,
    size_t capacity)
{
    const char *prefix =
        version[0] == 'v' ? "releases/" : "releases/v";

    int n = snprintf(output, capacity,
                     "%s%s", prefix, version);

    return n >= 0 && (size_t)n < capacity ? 0 : -1;
}

static int read_current(
    int dirfd,
    char *output,
    size_t capacity)
{
    if (capacity < 2)
        return -1;

    ssize_t n = readlinkat(
        dirfd, "current", output, capacity - 1);

    if (n < 0 || (size_t)n >= capacity - 1)
        return -1;

    output[n] = '\0';
    return 0;
}

/*
 * 校验已确认版本中的实际 Gateway ELF。
 *
 * 注意：
 * 这里只验证文件与 Manifest 的一致性，
 * 可信来源和版本身份绑定还需后续加强。
 */
static int check_confirmed(
    const char *install_dir,
    const char *target)
{
    char release_path[PATH_MAX];

    if (install_dir == NULL || target == NULL)
        return -1;

    int n = snprintf(
        release_path,
        sizeof(release_path),
        "%s/%s",
        install_dir,
        target
    );

    if (n < 0 ||
        (size_t)n >= sizeof(release_path)) {
        fprintf(stderr,
                "FAIL: confirmed release path too long\n");
        return -1;
    }

    if (ota_verify_package(release_path) != 0) {
        fprintf(stderr,
                "FAIL: confirmed ELF verification failed\n");
        return -1;
    }

    return 0;
}


/* 原子恢复 current，并同步所在目录 */
static int restore_current(
    int dirfd,
    const char *confirmed_target)
{
    char existing[128];

    if (symlinkat(
            confirmed_target,
            dirfd,
            RECOVER_TEMP_LINK) != 0) {

        if (errno != EEXIST)
            return -1;

        /*
         * 上次恢复可能已创建临时链接，
         * 仅在目标完全相同时继续使用。
         */
        ssize_t n = readlinkat(
            dirfd,
            RECOVER_TEMP_LINK,
            existing,
            sizeof(existing) - 1);

        if (n < 0 ||
            (size_t)n >= sizeof(existing) - 1)
            return -1;

        existing[n] = '\0';

        if (strcmp(existing, confirmed_target) != 0)
            return -1;
    }

    if (renameat(
            dirfd, RECOVER_TEMP_LINK,
            dirfd, "current") != 0)
        return -1;

    if (fsync(dirfd) != 0)
        return -1;

    if (read_current(
            dirfd, existing, sizeof(existing)) != 0)
        return -1;

    return strcmp(existing, confirmed_target) == 0
        ? 0 : -1;
}

/*
 * 通过公共状态机转换，并持久化新状态。
 * 若保存失败，必须重新读取磁盘状态再恢复。
 *
 * 为什么是"取出来 -> 转换 -> 放回去"这三步，而不是直接改 record->state:
 *
 *   1) 类型不同，不能直接传。
 *      ota_state_transition 的参数是 ota_context_t *，
 *      record->state 是 ota_state_t，两者不能互相取地址强转。
 *      这里的三步就是两个类型之间的"适配器"。
 *
 *   2) 转换可能失败，此时必须保证 record 完全没被改动。
 *      ota_state_transition 内部先调 ota_state_can_transition 校验，
 *      只有校验通过才写 ctx->state；所以失败时 ctx 仍是原值，
 *      而下面那句 record->state = ctx.state 只在成功路径上执行。
 *      如果绕过状态机直接写 record->state = next，
 *      非法转换（如 IDLE -> ACTIVATED）就会被静默接受并落盘。
 *
 *   3) 落盘需要完整 record，不只是 state。
 *      ota_state_save 会校验并写出 state + confirmed_version + candidate_version，
 *      因此 state 的更新必须回到 record 上，和两个版本号一起写。
 *
 * 为什么 ctx 在这里创建，而不是在 ota_recover_execute / main 里创建后传进来:
 *   当前的 ctx 只是 record->state 的一份"临时镜像"，本身不携带任何
 *   记录里没有的信息。如果在 main 里建一个长期存在的 ctx，就会出现
 *   record.state 和 ctx.state 两份副本，每次都必须手动同步:
 *       ctx.state = record.state;   // 用前同步
 *       record->state = ctx->state; // 用后同步
 *   漏掉任一处就会用到过期状态，这是典型的"双份真值"缺陷。
 *   现在每个转换都从 record 现场构造 ctx，用完立刻丢弃，
 *   真值只有 record 一份，没有同步问题。
 *
 *   什么时候才该把 ctx 提到上层: 当 ctx 开始承载"不需要落盘"的信息
 *   （如本次启动的重试次数、A/B 槽位、健康检查结果）时，
 *   它才真正成为跨多次转换存活的会话对象，那时由上层持有才合理。
 *
 * 一句话: 状态机的合法性规则只在 ota_state.c 里维护一份，
 *         所有调用方都必须走 ctx 这条路，不能自己改状态字段。
 */
static int persist_transition(
    const char *state_dir,
    ota_state_record_t *record,
    ota_state_t next)
{
    /* 第一步: 把磁盘记录里的当前状态搬进运行时上下文 */
    ota_context_t ctx = {
        .state = record->state
    };

    /* 第二步: 交给状态机校验并计算新状态；失败则 record 保持不变 */
    if (ota_state_transition(&ctx, next) != 0)
        return -1;

    /* 第三步: 校验通过，把新状态写回记录，再连同版本号一起落盘 */
    record->state = ctx.state;

    return ota_state_save(state_dir, record);
}

int ota_recover_execute(const char *install_dir)
{
    char state_dir[PATH_MAX];
    char confirmed_target[128];
    char candidate_target[128];
    char current[128];

    ota_state_record_t record = {0};

    int dirfd = -1;
    int result = 1;

    if (install_dir == NULL || install_dir[0] == '\0') {
        fprintf(stderr, "FAIL: invalid install directory\n");
        return 1;
    }

    int n = snprintf(
        state_dir, sizeof(state_dir),
        "%s/state", install_dir);

    if (n < 0 || (size_t)n >= sizeof(state_dir)) {
        fprintf(stderr, "FAIL: state path too long\n");
        return 1;
    }

    dirfd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY |
        O_NOFOLLOW | O_CLOEXEC);

    if (dirfd < 0) {
        perror("open lab directory");
        return 1;
    }

    /* 统一通过公共接口读取完整状态 */
    if (ota_state_load(state_dir, &record) != 0) {
        fprintf(stderr, "FAIL: cannot load OTA state\n");
        goto cleanup;
    }

    if (build_target(
            record.confirmed_version,
            confirmed_target,
            sizeof(confirmed_target)) != 0 ||
        build_target(
            record.candidate_version,
            candidate_target,
            sizeof(candidate_target)) != 0) {
        fprintf(stderr, "FAIL: invalid version path\n");
        goto cleanup;
    }

    if (check_confirmed(
            install_dir, confirmed_target) != 0) {
        fprintf(stderr,
                "FAIL: confirmed release unavailable\n");
        goto cleanup;
    }

    if (read_current(
            dirfd, current, sizeof(current)) != 0) {
        fprintf(stderr, "FAIL: cannot read current\n");
        goto cleanup;
    }

    int is_old =
        strcmp(current, confirmed_target) == 0;

    int is_new =
        strcmp(current, candidate_target) == 0;

    if (!is_old && !is_new) {
        fprintf(stderr,
                "FAIL: current points to unknown release\n");
        goto cleanup;
    }

    if (record.state == OTA_ROLLED_BACK) {
        if (!is_old) {
            fprintf(stderr,
                    "FAIL: rollback state inconsistent\n");
            goto cleanup;
        }

        puts("PASS: already rolled back (idempotent)");
        result = 0;
        goto cleanup;
    }

    if (record.state == OTA_CONFIRMED) {
        if (!is_old) {
            fprintf(stderr,
                    "FAIL: confirmed state inconsistent\n");
            goto cleanup;
        }

        puts("PASS: confirmed version is active");
        result = 0;
        goto cleanup;
    }

    /*
     * 这四种状态允许进入回滚流程。
     * 其他状态拒绝自动修改 current。
     */
    if (record.state != OTA_DOWNLOADED &&
        record.state != OTA_VERIFIED &&
        record.state != OTA_STAGED &&
        record.state != OTA_ACTIVATED &&
        record.state != OTA_HEALTHY &&
        record.state != OTA_ROLLBACK_PENDING) {
        fprintf(stderr, "FAIL: unsupported recovery state\n");
        goto cleanup;
    }

    if (is_new && strcmp(
            confirmed_target, candidate_target) == 0) {
        fprintf(stderr,
                "FAIL: ambiguous version targets\n");
        goto cleanup;
    }

    printf(
        "RECOVERY: state=%s, current=%s\n",
        ota_state_name(record.state),
        current);

    /*
     * 先持久化回滚意图，再恢复软链接。
     */
    if (record.state != OTA_ROLLBACK_PENDING) {
        if (persist_transition(
                state_dir,
                &record,
                OTA_ROLLBACK_PENDING) != 0) {
            fprintf(stderr,
                    "FAIL: cannot persist rollback intent\n");
            goto cleanup;
        }
    }

    if (is_new) {
        if (restore_current(
                dirfd, confirmed_target) != 0) {
            fprintf(stderr,
                    "FAIL: cannot restore current\n");
            goto cleanup;
        }

        puts("RECOVERY: current restored");
    }

    /*
     * 最后确认回滚完成。
     * 如果此处中断，下次可从 ROLLBACK_PENDING 继续。
     */
    if (persist_transition(
            state_dir,
            &record,
            OTA_ROLLED_BACK) != 0) {
        fprintf(stderr,
                "FAIL: cannot persist ROLLED_BACK\n");
        goto cleanup;
    }

    puts("PASS: rollback completed");
    result = 0;

cleanup:
    if (dirfd >= 0)
        close(dirfd);

    return result;
}
