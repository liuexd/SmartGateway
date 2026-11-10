#define _POSIX_C_SOURCE 200809L

#include "ota_oplog.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define OTA_OPLOG_NAME "last_operation.json"
#define OTA_OPLOG_TEMP "last_operation.json.tmp"
#define OTA_OPLOG_MAX 1024

/* ------------------------------------------------------------------ */
/* phase 名称表                                                        */
/* ------------------------------------------------------------------ */

static const struct {
    ota_oplog_phase_t phase;
    const char *name;
} phase_names[] = {
    { OTA_OPLOG_NONE,         "NONE"         },
    { OTA_OPLOG_STOPPING_OLD, "STOPPING_OLD" },
    { OTA_OPLOG_SWITCHING,    "SWITCHING"    },
    { OTA_OPLOG_STARTING_NEW, "STARTING_NEW" },
    { OTA_OPLOG_HEALTH_CHECK, "HEALTH_CHECK" },
    { OTA_OPLOG_ROLLING_BACK, "ROLLING_BACK" },
    { OTA_OPLOG_DONE,         "DONE"         },
    { OTA_OPLOG_FAILED,       "FAILED"       },
};

#define PHASE_COUNT \
    (sizeof(phase_names) / sizeof(phase_names[0]))

const char *ota_oplog_phase_name(ota_oplog_phase_t phase)
{
    for (size_t i = 0; i < PHASE_COUNT; i++) {
        if (phase_names[i].phase == phase)
            return phase_names[i].name;
    }

    return "UNKNOWN";
}

int ota_oplog_phase_parse(
    const char *name,
    ota_oplog_phase_t *out)
{
    if (name == NULL || out == NULL)
        return -1;

    for (size_t i = 0; i < PHASE_COUNT; i++) {
        if (strcmp(phase_names[i].name, name) == 0) {
            *out = phase_names[i].phase;
            return 0;
        }
    }

    return -1;
}

/*
 * 哪些 phase 算"已正常收尾"。
 *
 * 只有 DONE 与 FAILED。
 *
 * 特别注意 ROLLING_BACK **不**算收尾：它是"正在回滚"，
 * 回滚本身也可能中断，此时状态更难判断，必须人工看。
 */
int ota_oplog_phase_is_terminal(ota_oplog_phase_t phase)
{
    return phase == OTA_OPLOG_DONE ||
           phase == OTA_OPLOG_FAILED;
}

/* ------------------------------------------------------------------ */
/* 路径                                                                */
/* ------------------------------------------------------------------ */

int ota_oplog_path(
    const char *install_dir,
    char *output,
    size_t capacity)
{
    if (install_dir == NULL || install_dir[0] == '\0' ||
        output == NULL || capacity == 0) {
        errno = EINVAL;
        return -1;
    }

    int n = snprintf(
        output, capacity,
        "%s/%s", install_dir, OTA_OPLOG_NAME);

    if (n < 0 || (size_t)n >= capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 写入                                                                */
/* ------------------------------------------------------------------ */

static int write_all(int fd, const char *data, size_t len)
{
    size_t total = 0;

    while (total < len) {
        ssize_t n = write(fd, data + total, len - total);

        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0)
            return -1;

        total += (size_t)n;
    }

    return 0;
}

int ota_oplog_write(
    const char *install_dir,
    const ota_oplog_record_t *record)
{
    char temp_path[PATH_MAX];
    char final_path[PATH_MAX];
    char payload[OTA_OPLOG_MAX];

    if (record == NULL)
        return -1;

    if (ota_oplog_path(
            install_dir, final_path,
            sizeof(final_path)) != 0)
        return -1;

    int n = snprintf(
        temp_path, sizeof(temp_path),
        "%s/%s", install_dir, OTA_OPLOG_TEMP);

    if (n < 0 || (size_t)n >= sizeof(temp_path)) {
        errno = ENAMETOOLONG;
        return -1;
    }

    /*
     * 手写 JSON 而非用 json-c：
     *   1. 本结构字段固定且都是数字/受控枚举名，
     *      不存在需要转义的任意字符串（boot_id 已校验为 UUID）。
     *   2. 保持与 ota_state_io 一致的风格，减少一处依赖。
     *   3. 输出可直接被人工阅读，便于故障复盘。
     *
     * format_version 用于将来扩展时识别旧记录。
     */
    int p = snprintf(
        payload, sizeof(payload),
        "{\n"
        "  \"format_version\": 1,\n"
        "  \"phase\": \"%s\",\n"
        "  \"pid\": %ld,\n"
        "  \"started_at\": %lld,\n"
        "  \"boot_id\": \"%s\"\n"
        "}\n",
        ota_oplog_phase_name(record->phase),
        (long)record->pid,
        record->started_at,
        record->boot_id);

    if (p < 0 || (size_t)p >= sizeof(payload)) {
        errno = EOVERFLOW;
        return -1;
    }

    /*
     * 原子替换。
     *
     * 固定临时名可以接受，因为这些调用点都在 .ota.lock 保护下
     * （main.c 的 upgrade/recover 命令均持排他锁）。
     * 这与 supervisor 的 pidfile 不同：后者可能被非 OTA 路径
     * 调用，因此需要进程私有临时名。
     */
    int fd = open(
        temp_path,
        O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
        0600);

    if (fd < 0)
        return -1;

    if (write_all(fd, payload, (size_t)p) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(temp_path);
        errno = saved;
        return -1;
    }

    /* 文件内容必须先落盘，否则 rename 后掉电可能得到空文件 */
    if (fsync(fd) != 0) {
        int saved = errno;
        (void)close(fd);
        (void)unlink(temp_path);
        errno = saved;
        return -1;
    }

    if (close(fd) != 0) {
        int saved = errno;
        (void)unlink(temp_path);
        errno = saved;
        return -1;
    }

    if (rename(temp_path, final_path) != 0) {
        int saved = errno;
        (void)unlink(temp_path);
        errno = saved;
        return -1;
    }

    /* rename 是目录操作，目录项也要落盘才算持久 */
    int dirfd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (dirfd >= 0) {
        (void)fsync(dirfd);
        (void)close(dirfd);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 读取                                                                */
/* ------------------------------------------------------------------ */

/*
 * 极简 JSON 字段提取。
 *
 * 不引入 json-c 的原因：本文件结构由我们自己写入，格式固定。
 * 但**必须**仍然做严格校验，否则一条损坏记录会被误读成
 * 某个合法 phase，从而掩盖真实的未完成状态。
 *
 * 返回 0 找到，-1 没找到。
 */
static int extract_string_field(
    const char *buffer,
    const char *key,
    char *output,
    size_t capacity)
{
    char pattern[64];

    int n = snprintf(
        pattern, sizeof(pattern),
        "\"%s\"", key);

    if (n < 0 || (size_t)n >= sizeof(pattern))
        return -1;

    const char *pos = strstr(buffer, pattern);

    if (pos == NULL)
        return -1;

    pos += (size_t)n;

    /* 跳过冒号与空白 */
    while (*pos == ' ' || *pos == '\t' ||
           *pos == '\n' || *pos == '\r' || *pos == ':')
        pos++;

    if (*pos != '"')
        return -1;

    pos++;

    const char *end = strchr(pos, '"');

    if (end == NULL)
        return -1;

    size_t len = (size_t)(end - pos);

    if (len + 1 > capacity)
        return -1;

    memcpy(output, pos, len);
    output[len] = '\0';

    return 0;
}

static int extract_number_field(
    const char *buffer,
    const char *key,
    long long *output)
{
    char pattern[64];

    int n = snprintf(
        pattern, sizeof(pattern),
        "\"%s\"", key);

    if (n < 0 || (size_t)n >= sizeof(pattern))
        return -1;

    const char *pos = strstr(buffer, pattern);

    if (pos == NULL)
        return -1;

    pos += (size_t)n;

    while (*pos == ' ' || *pos == '\t' ||
           *pos == '\n' || *pos == '\r' || *pos == ':')
        pos++;

    char *end = NULL;

    errno = 0;
    long long value = strtoll(pos, &end, 10);

    /*
     * ERANGE 必须拒绝。
     * glibc 的 strtoll 溢出时返回 LLONG_MAX 且不报错，
     * 若不检查就接受，一个被破坏的数字字段会变成极大值。
     */
    if (errno != 0 || end == pos)
        return -1;

    *output = value;
    return 0;
}

int ota_oplog_read(
    const char *install_dir,
    ota_oplog_record_t *record_out,
    int *present)
{
    char path[PATH_MAX];
    char buffer[OTA_OPLOG_MAX + 1];

    if (record_out == NULL || present == NULL)
        return -1;

    *present = 0;
    memset(record_out, 0, sizeof(*record_out));

    if (ota_oplog_path(
            install_dir, path, sizeof(path)) != 0)
        return -1;

    /* O_NOFOLLOW：这条记录不该是符号链接 */
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0) {
        if (errno == ENOENT)
            return 0;      /* 无记录：正常情况 */

        return -1;
    }

    struct stat st;

    if (fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        st.st_size > OTA_OPLOG_MAX) {
        (void)close(fd);
        errno = EINVAL;
        return -1;
    }

    size_t total = 0;

    while (total < (size_t)st.st_size) {
        ssize_t r = read(
            fd, buffer + total,
            (size_t)st.st_size - total);

        if (r < 0 && errno == EINTR)
            continue;

        if (r <= 0) {
            (void)close(fd);
            errno = EIO;
            return -1;
        }

        total += (size_t)r;
    }

    (void)close(fd);
    buffer[total] = '\0';

    char phase_name[32];

    if (extract_string_field(
            buffer, "phase",
            phase_name, sizeof(phase_name)) != 0)
        return -1;

    if (ota_oplog_phase_parse(
            phase_name, &record_out->phase) != 0)
        return -1;

    long long pid_value = 0;
    long long started = 0;

    if (extract_number_field(
            buffer, "pid", &pid_value) != 0)
        return -1;

    if (extract_number_field(
            buffer, "started_at", &started) != 0)
        return -1;

    record_out->pid = (pid_t)pid_value;
    record_out->started_at = started;

    /* boot_id 可缺省（不影响主判断），但存在时必须能读出来 */
    if (extract_string_field(
            buffer, "boot_id",
            record_out->boot_id,
            sizeof(record_out->boot_id)) != 0) {
        record_out->boot_id[0] = '\0';
    }

    *present = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* 删除                                                                */
/* ------------------------------------------------------------------ */

int ota_oplog_remove(const char *install_dir)
{
    char path[PATH_MAX];

    if (ota_oplog_path(
            install_dir, path, sizeof(path)) != 0)
        return -1;

    if (unlink(path) != 0) {
        if (errno == ENOENT)
            return 0;

        return -1;
    }

    /*
     * 目录项落盘：否则掉电后已删除的记录可能"复活"，
     * 导致下次启动误判有未完成操作。
     */
    int dirfd = open(
        install_dir,
        O_RDONLY | O_DIRECTORY | O_CLOEXEC);

    if (dirfd >= 0) {
        (void)fsync(dirfd);
        (void)close(dirfd);
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* 便捷封装                                                            */
/* ------------------------------------------------------------------ */

int ota_oplog_begin(
    const char *install_dir,
    ota_oplog_phase_t phase)
{
    ota_oplog_record_t record = {0};

    record.phase = phase;
    record.pid = getpid();
    record.started_at = (long long)time(NULL);

    /*
     * boot_id 读取失败不影响写入：它只是辅助判断
     * "这条记录是不是本次开机留下的"，缺失时留空即可。
     */
    int fd = open(
        "/proc/sys/kernel/random/boot_id",
        O_RDONLY | O_CLOEXEC);

    if (fd >= 0) {
        char buf[64];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);

        if (n > 0) {
            buf[n] = '\0';

            /* 去掉尾部换行 */
            for (ssize_t i = 0; i < n; i++) {
                if (buf[i] == '\n' || buf[i] == '\r') {
                    buf[i] = '\0';
                    break;
                }
            }

            /* 只接受内核标准格式（36 字符 UUID） */
            if (strlen(buf) == 36)
                memcpy(record.boot_id, buf, 37);
        }

        (void)close(fd);
    }

    return ota_oplog_write(install_dir, &record);
}
