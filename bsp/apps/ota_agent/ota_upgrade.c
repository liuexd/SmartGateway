
#define _POSIX_C_SOURCE 200809L

#include "ota_upgrade.h"
#include "ota_verify.h"
#include "ota_state_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <json-c/json.h>

#include "ota_json_compat.h"

#define MANIFEST_MAX 4096
#define VERSION_MAX 64
#define TEMP_LINK "current.activate.next"

typedef struct {
    char version[VERSION_MAX];
    char sha256[65];
} package_identity_t;

/*
 * 从 Manifest 读取版本和 SHA-256。
 * 调用此函数前应先通过 ota_verify_package()。
 */
static int read_identity(
    const char *directory,
    package_identity_t *identity)
{
    char path[PATH_MAX];
    char buffer[MANIFEST_MAX + 1];
    struct stat st;
    json_tokener *tok = NULL;
    json_object *root = NULL;
    json_object *field = NULL;
    const char *value;
    size_t total = 0;
    int fd = -1;
    int result = -1;

    int n = snprintf(
        path, sizeof(path),
        "%s/manifest.json", directory);

    if (n < 0 || (size_t)n >= sizeof(path))
        return -1;

    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        goto out;

    if (fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        st.st_size > MANIFEST_MAX)
        goto out;

    while (total < (size_t)st.st_size) {
        ssize_t r = read(
            fd, buffer + total,
            (size_t)st.st_size - total);

        if (r < 0 && errno == EINTR)
            continue;

        if (r <= 0)
            goto out;

        total += (size_t)r;
    }

    buffer[total] = '\0';

    tok = json_tokener_new();
    if (!tok)
        goto out;

    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    root = json_tokener_parse_ex(tok, buffer, (int)total);

    if (json_tokener_get_error(tok) != json_tokener_success ||
        !root ||
        json_object_get_type(root) != json_type_object ||
        ota_json_parse_end(tok) != total)
        goto out;

    if (!json_object_object_get_ex(
            root, "version", &field) ||
        json_object_get_type(field) != json_type_string)
        goto out;

    value = json_object_get_string(field);
    size_t len = (size_t)json_object_get_string_len(field);

    if (!value || len == 0 || len >= VERSION_MAX ||
        strlen(value) != len ||
        value[0] < '0' || value[0] > '9')
        goto out;

    for (size_t i = 0; i < len; i++) {
        char c = value[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'z') ||
              (c >= 'A' && c <= 'Z') ||
              c == '.' || c == '-' || c == '_'))
            goto out;
    }

    memcpy(identity->version, value, len + 1);

    if (!json_object_object_get_ex(
            root, "sha256", &field) ||
        json_object_get_type(field) != json_type_string)
        goto out;

    value = json_object_get_string(field);
    len = (size_t)json_object_get_string_len(field);

    if (!value || len != 64 || strlen(value) != 64)
        goto out;

    for (size_t i = 0; i < 64; i++) {
        char c = value[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f')))
            goto out;
    }

    memcpy(identity->sha256, value, 65);
    result = 0;

out:
    if (root)
        json_object_put(root);
    if (tok)
        json_tokener_free(tok);
    if (fd >= 0)
        close(fd);

    return result;
}

static int release_path(
    const char *install_dir,
    const char *version,
    char *output,
    size_t size)
{
    int n = snprintf(output, size,
                     "%s/releases/v%s",
                     install_dir, version);

    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static int release_target(
    const char *version,
    char *output,
    size_t size)
{
    int n = snprintf(output, size,
                     "releases/v%s", version);

    return n >= 0 && (size_t)n < size ? 0 : -1;
}

static int read_current(
    int dirfd, char *output, size_t size)
{
    ssize_t n = readlinkat(
        dirfd, "current", output, size - 1);

    if (n < 0 || (size_t)n >= size - 1)
        return -1;

    output[n] = '\0';
    return 0;
}

/* 合法转换后持久化，成功后才更新内存状态 */
static int advance_state(
    const char *state_dir,
    ota_state_record_t *record,
    ota_state_t next_state)
{
    ota_state_record_t updated = *record;
    ota_context_t context = {
        .state = updated.state
    };

    if (ota_state_transition(&context, next_state) != 0)
        return -1;

    updated.state = context.state;

    if (ota_state_save(state_dir, &updated) != 0)
        return -1;

    *record = updated;
    return 0;
}

/* 原子替换 current 软链接 */
static int activate_current(
    int dirfd,
    const char *target)
{
    char existing[128];

    if (symlinkat(target, dirfd, TEMP_LINK) != 0) {
        if (errno != EEXIST)
            return -1;

        ssize_t n = readlinkat(
            dirfd, TEMP_LINK,
            existing, sizeof(existing) - 1);

        if (n < 0 ||
            (size_t)n >= sizeof(existing) - 1)
            return -1;

        existing[n] = '\0';

        if (strcmp(existing, target) != 0)
            return -1;
    }

    if (renameat(
            dirfd, TEMP_LINK,
            dirfd, "current") != 0)
        return -1;

    if (fsync(dirfd) != 0)
        return -1;

    if (read_current(
            dirfd, existing, sizeof(existing)) != 0)
        return -1;

    return strcmp(existing, target) == 0 ? 0 : -1;
}

/*
 * activate_check - 激活前的**全部只读校验**（S3-R2）
 *
 * 这个函数不修改任何状态：不写状态文件、不切换 current、不删文件。
 * 它只回答一个问题："这次激活能否成功？"
 *
 * 为什么必须独立出来：
 *   ota_activate_test() 原本把校验和变更混在一起，导致调用方
 *   无法在"产生副作用之前"判断能否升级。典型后果是：
 *   升级包合法但 releases/<ver> 不存在（未 stage），
 *   旧 Gateway 已被停掉，activate 才发现候选缺失 —— 业务白白中断。
 *
 * 校验清单（与 ota_activate_test 的要求一一对应）：
 *   1. 状态必须是 CONFIRMED 或 ROLLED_BACK（无进行中的事务）
 *   2. current 必须指向 confirmed 版本
 *   3. 升级包本身合法（ota_verify_package：manifest/size/SHA-256/ELF）
 *   4. 候选版本不能等于已确认版本
 *   5. 已确认版本的文件完整、版本号自洽
 *   6. 候选版本**已经安装**且与升级包 SHA-256 一致
 *
 * source_out 非空时输出从升级包读到的身份，供调用方复用，
 * 避免重复解析同一份 manifest。
 *
 * 返回 0 表示可以安全进入变更阶段；-1 表示不可升级。
 */
static int activate_check(
    const char *package_dir,
    const char *install_dir,
    package_identity_t *source_out)
{
    char state_dir[PATH_MAX];
    char old_path[PATH_MAX];
    char new_path[PATH_MAX];
    char old_target[128];
    char current[128];

    package_identity_t source = {0};
    package_identity_t installed = {0};
    package_identity_t confirmed = {0};
    ota_state_record_t record = {0};

    int dirfd = -1;
    int result = -1;

    if (!package_dir || !install_dir)
        return -1;

    int n = snprintf(
        state_dir, sizeof(state_dir),
        "%s/state", install_dir);

    if (n < 0 || (size_t)n >= sizeof(state_dir))
        return -1;

    dirfd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY |
        O_NOFOLLOW | O_CLOEXEC);

    if (dirfd < 0)
        goto out;

    if (ota_state_load(state_dir, &record) != 0)
        goto out;

    /* 检查 1：不得有进行中的事务 */
    if (record.state != OTA_CONFIRMED &&
        record.state != OTA_ROLLED_BACK) {
        fprintf(stderr, "FAIL: OTA transaction already active\n");
        goto out;
    }

    /* 检查 2：current 必须指向已确认版本 */
    if (release_target(
            record.confirmed_version,
            old_target, sizeof(old_target)) != 0 ||
        read_current(dirfd, current, sizeof(current)) != 0 ||
        strcmp(current, old_target) != 0) {
        fprintf(stderr, "FAIL: confirmed/current mismatch\n");
        goto out;
    }

    /* 检查 3+4：包合法，且候选不等于已确认版本 */
    if (ota_verify_package(package_dir) != 0 ||
        read_identity(package_dir, &source) != 0)
        goto out;

    if (strcmp(
            source.version, record.confirmed_version) == 0) {
        fprintf(stderr, "FAIL: candidate equals confirmed\n");
        goto out;
    }

    if (release_path(
            install_dir, record.confirmed_version,
            old_path, sizeof(old_path)) != 0 ||
        release_path(
            install_dir, source.version,
            new_path, sizeof(new_path)) != 0)
        goto out;

    /* 检查 5：已确认版本的完整性与自洽 */
    if (ota_verify_package(old_path) != 0 ||
        read_identity(old_path, &confirmed) != 0 ||
        strcmp(
            confirmed.version,
            record.confirmed_version) != 0) {
        fprintf(stderr, "FAIL: confirmed release invalid\n");
        goto out;
    }

    /*
     * 检查 6：候选版本**必须已经安装**（已 stage），且与升级包一致。
     *
     * 这一条正是本次复审指出的遗漏：只检查 releases 目录存在
     * 是不够的，必须确认 releases/<candidate> 真的在那里且内容相符。
     */
    if (ota_verify_package(new_path) != 0 ||
        read_identity(new_path, &installed) != 0 ||
        strcmp(source.version, installed.version) != 0 ||
        strcmp(source.sha256, installed.sha256) != 0) {
        fprintf(stderr, "FAIL: installed candidate mismatch\n");
        goto out;
    }

    if (source_out != NULL)
        *source_out = source;

    result = 0;

out:
    if (dirfd >= 0)
        close(dirfd);

    return result;
}

int ota_activate_precheck(
    const char *package_dir,
    const char *install_dir)
{
    /*
     * 对外暴露的只读校验入口。
     *
     * 调用方（ota_upgrade_run.c 的 preflight_package）用它
     * 在**停止旧 Gateway 之前**确认升级能否成功。
     *
     * 注意：必须与 ota_activate_test() 使用同一份校验实现，
     * 否则两处会随时间分叉——precheck 放行的包可能在 activate
     * 阶段被拒，业务中断的问题就会重新出现。
     */
    return activate_check(package_dir, install_dir, NULL);
}

int ota_activate_test(
    const char *package_dir,
    const char *install_dir)
{
    char state_dir[PATH_MAX];
    char new_target[128];

    package_identity_t source = {0};
    ota_state_record_t record = {0};

    int dirfd = -1;
    int result = -1;

    if (!package_dir || !install_dir)
        return -1;

    int n = snprintf(
        state_dir, sizeof(state_dir),
        "%s/state", install_dir);

    if (n < 0 || (size_t)n >= sizeof(state_dir))
        return -1;

    /*
     * 第一步：完整只读校验。
     *
     * 与 ota_activate_precheck() 是同一个函数，
     * 因此"预检通过但激活失败"不可能由校验规则差异导致。
     */
    if (activate_check(package_dir, install_dir, &source) != 0)
        goto out;

    dirfd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY |
        O_NOFOLLOW | O_CLOEXEC);

    if (dirfd < 0)
        goto out;

    if (ota_state_load(state_dir, &record) != 0)
        goto out;

    if (release_target(
            source.version,
            new_target, sizeof(new_target)) != 0)
        goto out;

    /* 以下才是变更阶段：从这里开始会产生持久化副作用 */

    /* 保存 DOWNLOADED，保留 confirmed_version */
    if (ota_state_begin_upgrade(
            state_dir, &record, source.version) != 0)
        goto out;

    if (advance_state(
            state_dir, &record, OTA_VERIFIED) != 0 ||
        advance_state(
            state_dir, &record, OTA_STAGED) != 0)
        goto out;

    /*
     * 中断窗口：
     * current 已切换，但状态可能仍为 STAGED。
     * 现有 recover 可以协调这一情况。
     */
    if (activate_current(dirfd, new_target) != 0)
        goto out;

    if (advance_state(
            state_dir, &record, OTA_ACTIVATED) != 0)
        goto out;

    printf("PASS: candidate activated: %s\n", source.version);
    puts("NOTICE: health check not performed; not confirmed");

    result = 0;

out:
    if (result != 0) {
        fprintf(stderr,
                "FAIL: activation incomplete; "
                "inspect status and recover before retry\n");
    }

    if (dirfd >= 0)
        close(dirfd);

    return result;
}
