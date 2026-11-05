
#define _POSIX_C_SOURCE 200809L

#include "ota_state_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <json-c/json.h>

#define OTA_STATE_FILE "ota_state.json"
#define OTA_STATE_MAX 4096

static int valid_version(const char *s)
{
    size_t n;

    if (s == NULL)
        return 0;

    n = strnlen(s, OTA_VERSION_MAX);

    if (n == 0 || n >= OTA_VERSION_MAX)
        return 0;

    /* 第一个字符必须是数字或英文字母 */
    if (!((s[0] >= '0' && s[0] <= '9') ||
          (s[0] >= 'A' && s[0] <= 'Z') ||
          (s[0] >= 'a' && s[0] <= 'z')))
        return 0;

    for (size_t i = 0; i < n; i++) {
        char c = s[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              c == '.' || c == '-' || c == '_'))
            return 0;
    }

    return 1;
}

static int valid_record(const ota_state_record_t *record)
{
    if (record == NULL)
        return 0;

    if (record->state < OTA_IDLE ||
        record->state >= OTA_STATE_COUNT)
        return 0;

    return valid_version(record->confirmed_version) &&
           valid_version(record->candidate_version);
}

static int write_all(int fd, const char *data, size_t length)
{
    size_t total = 0;

    while (total < length) {
        ssize_t n = write(fd, data + total, length - total);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            if (n == 0)
                errno = EIO;
            return -1;
        }

        total += (size_t)n;
    }

    return 0;
}

static int add_json_string(
    json_object *root,
    const char *key,
    const char *value)
{
    json_object *item = json_object_new_string(value);

    if (item == NULL)
        return -1;

    if (json_object_object_add(root, key, item) != 0) {
        json_object_put(item);
        return -1;
    }

    return 0;
}

int ota_state_save(
    const char *directory,
    const ota_state_record_t *record)
{
    char temp_path[PATH_MAX];
    char state_path[PATH_MAX];

    json_object *root = NULL;
    json_object *schema = NULL;

    int dirfd = -1;
    int fd = -1;
    int temp_created = 0;
    int result = -1;

    if (directory == NULL || !valid_record(record)) {
        errno = EINVAL;
        return -1;
    }

    dirfd = open(directory,
                 O_RDONLY | O_DIRECTORY |
                 O_CLOEXEC | O_NOFOLLOW);

    if (dirfd < 0)
        goto cleanup;

    int a = snprintf(
        temp_path, sizeof(temp_path),
        "%s/.ota_state.tmp.XXXXXX", directory);

    int b = snprintf(
        state_path, sizeof(state_path),
        "%s/%s", directory, OTA_STATE_FILE);

    if (a < 0 || (size_t)a >= sizeof(temp_path) ||
        b < 0 || (size_t)b >= sizeof(state_path)) {
        errno = ENAMETOOLONG;
        goto cleanup;
    }

    root = json_object_new_object();
    schema = json_object_new_int(1);

    if (root == NULL || schema == NULL)
        goto cleanup;

    if (json_object_object_add(
            root, "schema_version", schema) != 0)
        goto cleanup;

    schema = NULL;

    if (add_json_string(
            root, "state",
            ota_state_name(record->state)) != 0 ||
        add_json_string(
            root, "confirmed_version",
            record->confirmed_version) != 0 ||
        add_json_string(
            root, "candidate_version",
            record->candidate_version) != 0)
        goto cleanup;

    const char *data = json_object_to_json_string_ext(
        root, JSON_C_TO_STRING_PRETTY);

    if (data == NULL || strlen(data) + 1 > OTA_STATE_MAX) {
        errno = EINVAL;
        goto cleanup;
    }

    fd = mkstemp(temp_path);
    if (fd < 0)
        goto cleanup;

    temp_created = 1;

    if (write_all(fd, data, strlen(data)) != 0 ||
        write_all(fd, "\n", 1) != 0)
        goto cleanup;

    /* 先持久化临时文件 */
    if (fsync(fd) != 0)
        goto cleanup;

    if (close(fd) != 0) {
        fd = -1;
        goto cleanup;
    }
    fd = -1;

    /* 同一目录内原子替换 */
    if (rename(temp_path, state_path) != 0)
        goto cleanup;

    temp_created = 0;

    /*
     * 注意：如果这里失败，rename 可能已经成功。
     * 调用方必须重新读取状态，不可假定文件未改变。
     */
    if (fsync(dirfd) != 0)
        goto cleanup;

    result = 0;

cleanup:
    if (fd >= 0)
        close(fd);

    if (temp_created)
        unlink(temp_path);

    if (dirfd >= 0)
        close(dirfd);

    if (schema != NULL)
        json_object_put(schema);

    if (root != NULL)
        json_object_put(root);

    return result;
}

static int get_version(
    json_object *root,
    const char *key,
    char output[OTA_VERSION_MAX])
{
    json_object *field = NULL;

    if (!json_object_object_get_ex(root, key, &field) ||
        json_object_get_type(field) != json_type_string)
        return -1;

    const char *value = json_object_get_string(field);
    size_t len = (size_t)json_object_get_string_len(field);

    if (value == NULL ||
        len == 0 ||
        len >= OTA_VERSION_MAX ||
        strlen(value) != len)
        return -1;

    memcpy(output, value, len + 1);

    return valid_version(output) ? 0 : -1;
}

int ota_state_load(
    const char *directory,
    ota_state_record_t *record)
{
    char buffer[OTA_STATE_MAX + 1];

    ota_state_record_t temp = {0};

    json_tokener *tok = NULL;
    json_object *root = NULL;
    json_object *schema = NULL;
    json_object *state_field = NULL;

    struct stat st;

    int dirfd = -1;
    int fd = -1;
    int result = -1;

    size_t total = 0;

    if (directory == NULL || record == NULL) {
        errno = EINVAL;
        return -1;
    }

    dirfd = open(directory,
                 O_RDONLY | O_DIRECTORY |
                 O_CLOEXEC | O_NOFOLLOW);

    if (dirfd < 0)
        goto cleanup;

    fd = openat(dirfd, OTA_STATE_FILE,
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0)
        goto cleanup;

    if (fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        st.st_size > OTA_STATE_MAX)
        goto cleanup;

    while (total < (size_t)st.st_size) {
        ssize_t n = read(
            fd, buffer + total,
            (size_t)st.st_size - total);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            goto cleanup;

        total += (size_t)n;
    }

    buffer[total] = '\0';

    tok = json_tokener_new();
    if (tok == NULL)
        goto cleanup;

    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);

    root = json_tokener_parse_ex(tok, buffer, (int)total);

    if (json_tokener_get_error(tok) != json_tokener_success ||
        root == NULL ||
        json_object_get_type(root) != json_type_object ||
        (size_t)json_tokener_get_parse_end(tok) != total)
        goto cleanup;

    if (!json_object_object_get_ex(
            root, "schema_version", &schema) ||
        json_object_get_type(schema) != json_type_int ||
        json_object_get_int64(schema) != 1)
        goto cleanup;

    if (!json_object_object_get_ex(
            root, "state", &state_field) ||
        json_object_get_type(state_field) != json_type_string)
        goto cleanup;

    if (ota_state_parse(
            json_object_get_string(state_field),
            &temp.state) != 0)
        goto cleanup;

    if (get_version(root, "confirmed_version",
                    temp.confirmed_version) != 0 ||
        get_version(root, "candidate_version",
                    temp.candidate_version) != 0)
        goto cleanup;

    /*
     * 全部校验成功后才修改调用者的 record。
     * 失败时不会留下半初始化的输出。
     */
    *record = temp;
    result = 0;

cleanup:
    if (fd >= 0)
        close(fd);

    if (dirfd >= 0)
        close(dirfd);

    if (root != NULL)
        json_object_put(root);

    if (tok != NULL)
        json_tokener_free(tok);

    return result;
}

int ota_state_begin_upgrade(
    const char *state_directory,
    ota_state_record_t *record,
    const char *candidate_version)
{
    ota_state_record_t next;
    ota_context_t ctx;

    if (state_directory == NULL ||
        record == NULL ||
        !valid_version(candidate_version)) {
        errno = EINVAL;
        return -1;
    }

    /* 使用不带 v 前缀的规范版本号 */
    if (candidate_version[0] < '0' ||
        candidate_version[0] > '9') {
        errno = EINVAL;
        return -1;
    }

    if (record->state != OTA_CONFIRMED &&
        record->state != OTA_ROLLED_BACK) {
        errno = EINVAL;
        return -1;
    }

    if (!valid_version(record->confirmed_version) ||
        strcmp(record->confirmed_version,
               candidate_version) == 0) {
        errno = EINVAL;
        return -1;
    }

    ctx.state = record->state;

    if (ota_state_transition(
            &ctx, OTA_DOWNLOADED) != 0) {
        errno = EINVAL;
        return -1;
    }

    /*
     * 使用局部副本，避免持久化前修改
     * 调用者持有的原始状态。
     */
    next = *record;
    next.state = ctx.state;

    size_t len = strlen(candidate_version);

    memcpy(next.candidate_version,
           candidate_version,
           len + 1);

    /* 先持久化，成功后再更新内存记录 */
    if (ota_state_save(state_directory, &next) != 0)
        return -1;

    *record = next;
    return 0;
}
