#define _GNU_SOURCE
#include "r3c_data.h"
#include "ota_oplog.h"
#include "ota_state_io.h"
#include "ota_json_compat.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <json-c/json.h>

static int fail(char *reason, size_t size, const char *message)
{
    if (reason && size) snprintf(reason, size, "%s", message);
    return -1;
}

static int safe_absolute(const char *s)
{
    if (!s || s[0] != '/' || s[1] == '\0' || strlen(s) >= PATH_MAX ||
        s[strlen(s)-1] == '/' || strstr(s, "//") || strstr(s, "/../") ||
        strstr(s, "/./") || strpbrk(s, " \t\n\r\\"))
        return 0;
    if (strlen(s) >= 3 && strcmp(s + strlen(s) - 3, "/..") == 0) return 0;
    if (strlen(s) >= 2 && strcmp(s + strlen(s) - 2, "/.") == 0) return 0;
    return 1;
}

static int has_option(const char *options, const char *wanted)
{
    size_t n = strlen(wanted);
    const char *p = options;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        if (len == n && memcmp(p, wanted, n) == 0) return 1;
        if (!end) break;
        p = end + 1;
    }
    return 0;
}

int r3c_data_mountinfo_matches(const char *text, const char *mountpoint,
                               unsigned int dev_major, unsigned int dev_minor)
{
    char *copy, *line, *save = NULL;
    int found = 0, good = 0;
    if (!text || !safe_absolute(mountpoint)) return -1;
    copy = strdup(text);
    if (!copy) return -1;
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        unsigned int maj, min;
        char root[PATH_MAX], mnt[PATH_MAX], opts[1024];
        char *separator = strstr(line, " - ");
        char type[64];
        if (!separator ||
            sscanf(line, "%*u %*u %u:%u %4095s %4095s %1023s",
                   &maj, &min, root, mnt, opts) != 5 ||
            sscanf(separator + 3, "%63s", type) != 1) {
            free(copy);
            return -1;
        }
        if (strcmp(mnt, mountpoint) != 0) continue;
        ++found;
        good = maj == dev_major && min == dev_minor &&
               strcmp(root, "/") == 0 && strcmp(type, "ext4") == 0 &&
               has_option(opts, "rw") && !has_option(opts, "ro");
    }
    free(copy);
    return found == 1 && good ? 0 : -1;
}

static int trusted_dirs(const char *path, uid_t owner)
{
    char buf[PATH_MAX];
    size_t i, n;
    struct stat st;
    if (!safe_absolute(path)) return -1;
    n = strlen(path);
    memcpy(buf, path, n + 1);
    if (lstat("/", &st) || st.st_uid != 0 || (st.st_mode & 0022)) return -1;
    for (i = 1; i <= n; ++i) {
        char old;
        if (buf[i] != '/' && buf[i] != '\0') continue;
        old = buf[i]; buf[i] = '\0';
        if (lstat(buf, &st) || !S_ISDIR(st.st_mode) ||
            (st.st_uid != 0 && st.st_uid != owner) || (st.st_mode & 0022))
            return -1;
        buf[i] = old;
    }
    return 0;
}

static int read_mountinfo(char **out)
{
    FILE *fp;
    char *line = NULL, *all = NULL;
    size_t cap = 0, used = 0;
    ssize_t n;
    fp = fopen("/proc/self/mountinfo", "re");
    if (!fp) return -1;
    while ((n = getline(&line, &cap, fp)) > 0) {
        char *next;
        if ((size_t)n > 1024 * 1024 - used) goto error;
        next = realloc(all, used + (size_t)n + 1);
        if (!next) goto error;
        all = next;
        memcpy(all + used, line, (size_t)n);
        used += (size_t)n;
        all[used] = '\0';
    }
    if (ferror(fp) || !all) goto error;
    free(line); fclose(fp);
    *out = all;
    return 0;
error:
    free(all); free(line); fclose(fp);
    return -1;
}

int r3c_data_inspect(const r3c_data_policy_t *p, char *reason, size_t size)
{
    struct stat dev, root, mnt, schema;
    struct statvfs vfs;
    char *mountinfo = NULL;
    char content[16];
    int dir = -1, fd = -1, rc = -1;
    ssize_t n;
    if (!p || !safe_absolute(p->mountpoint) || !safe_absolute(p->device) ||
        p->minimum_available_bytes == 0)
        return fail(reason, size, "invalid or incomplete policy");
    if (trusted_dirs(p->mountpoint, p->owner) != 0 ||
        stat(p->device, &dev) != 0 || !S_ISBLK(dev.st_mode) ||
        stat("/", &root) != 0)
        return fail(reason, size, "untrusted mount path or block device");
    dir = open(p->mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0 || fstat(dir, &mnt) != 0 || mnt.st_dev != dev.st_rdev ||
        mnt.st_dev == root.st_dev) {
        fail(reason, size, "mount device does not match or is root"); goto out;
    }
    if (read_mountinfo(&mountinfo) != 0 ||
        r3c_data_mountinfo_matches(mountinfo, p->mountpoint,
                                   major(dev.st_rdev), minor(dev.st_rdev)) != 0) {
        fail(reason, size, "mountinfo identity or mode invalid"); goto out;
    }
    if (fstatvfs(dir, &vfs) != 0 || (vfs.f_flag & ST_RDONLY) ||
        vfs.f_frsize == 0 ||
        vfs.f_bavail < (p->minimum_available_bytes / vfs.f_frsize +
                        (p->minimum_available_bytes % vfs.f_frsize != 0)) ||
        faccessat(dir, ".", W_OK, AT_EACCESS) != 0) {
        fail(reason, size, "DATA read-only, inaccessible or low on space"); goto out;
    }
    fd = openat(dir, ".sgw-data-schema", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &schema) != 0 || !S_ISREG(schema.st_mode) ||
        schema.st_uid != p->owner || (schema.st_mode & 0022) || schema.st_size != 9) {
        fail(reason, size, "missing or untrusted DATA schema"); goto out;
    }
    n = read(fd, content, sizeof(content));
    if (n != 9 || memcmp(content, "schema=1\n", 9) != 0) {
        fail(reason, size, "unsupported or invalid DATA schema"); goto out;
    }
    if (reason && size) snprintf(reason, size, "DATA prerequisites observed");
    rc = 0;
out:
    free(mountinfo);
    if (fd >= 0) close(fd);
    if (dir >= 0) close(dir);
    return rc;
}

static int secure_node(int dir, const char *name, uid_t owner, int directory)
{
    struct stat st;
    if (fstatat(dir, name, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
        st.st_uid != owner || (st.st_mode & 0022)) return -1;
    return (directory ? S_ISDIR(st.st_mode) : S_ISREG(st.st_mode)) ? 0 : -1;
}

/* The historic parsers do not reject duplicate JSON keys. Only the simple
 * on-disk format emitted by the OTA agent is accepted by this diagnostic:
 * no escapes and exactly one literal occurrence of each required key. */
static int unique_json_keys(int dir, const char *name,
                            const char *const *keys, size_t key_count)
{
    char buf[4097];
    struct stat st;
    size_t i;
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    ssize_t n;
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > 4096) { close(fd); return -1; }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n != st.st_size || memchr(buf, '\0', (size_t)n) ||
        memchr(buf, '\\', (size_t)n)) return -1;
    buf[n] = '\0';
    for (i = 0; i < key_count; ++i) {
        const char *first = strstr(buf, keys[i]);
        if (!first || strstr(first + strlen(keys[i]), keys[i])) return -1;
    }
    return 0;
}

/* The legacy oplog reader extracts fields with strstr; require valid JSON
 * and compare security-relevant fields before trusting its terminal result. */
static int strict_oplog(int dir, const ota_oplog_record_t *record)
{
    char buf[1025];
    struct stat st;
    json_tokener *tok = NULL;
    json_object *obj = NULL, *field = NULL;
    int fd = -1, rc = -1;
    ssize_t n;
    fd = openat(dir, "last_operation.json",
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size <= 0 || st.st_size > 1024) goto out;
    n = read(fd, buf, sizeof(buf) - 1);
    if (n != st.st_size || memchr(buf, '\0', (size_t)n)) goto out;
    tok = json_tokener_new();
    if (!tok) goto out;
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    obj = json_tokener_parse_ex(tok, buf, (int)n);
    if (json_tokener_get_error(tok) != json_tokener_success || !obj ||
        json_object_get_type(obj) != json_type_object ||
        ota_json_parse_end(tok) != (size_t)n) goto out;
    if (!json_object_object_get_ex(obj, "format_version", &field) ||
        json_object_get_type(field) != json_type_int ||
        json_object_get_int64(field) != 1) goto out;
    if (!json_object_object_get_ex(obj, "phase", &field) ||
        json_object_get_type(field) != json_type_string ||
        (size_t)json_object_get_string_len(field) != strlen(json_object_get_string(field)) ||
        strcmp(json_object_get_string(field), ota_oplog_phase_name(record->phase)) != 0)
        goto out;
    if (!json_object_object_get_ex(obj, "pid", &field) ||
        json_object_get_type(field) != json_type_int ||
        json_object_get_int64(field) != (long long)record->pid) goto out;
    if (!json_object_object_get_ex(obj, "started_at", &field) ||
        json_object_get_type(field) != json_type_int ||
        json_object_get_int64(field) != record->started_at) goto out;
    rc = 0;
out:
    if (obj) json_object_put(obj);
    if (tok) json_tokener_free(tok);
    if (fd >= 0) close(fd);
    return rc;
}

int r3c_app_ota_inspect(const char *path, uid_t owner, char *reason, size_t size)
{
    static const char *const state_keys[] = {
        "\"schema_version\"", "\"state\"", "\"confirmed_version\"", "\"candidate_version\""
    };
    static const char *const oplog_keys[] = {
        "\"format_version\"", "\"phase\"", "\"pid\"", "\"started_at\""
    };
    ota_state_record_t state;
    ota_oplog_record_t oplog;
    struct stat st;
    char statepath[PATH_MAX], current[128], expected[128];
    int dir = -1, lock = -1, statefd = -1, releases = -1, release = -1;
    int present = 0, rc = -1, n;
    ssize_t len;
    if (trusted_dirs(path, owner) != 0)
        return fail(reason, size, "untrusted application directory");
    dir = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0 || fstat(dir, &st) != 0 || st.st_uid != owner) goto invalid;
    /* Never create a lock file in a diagnostic. */
    lock = openat(dir, ".ota.lock", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (lock < 0 || fstat(lock, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != owner || (st.st_mode & 0022) || flock(lock, LOCK_SH | LOCK_NB) != 0)
        goto invalid;
    if (secure_node(dir, "state", owner, 1) != 0 ||
        secure_node(dir, "releases", owner, 1) != 0) goto invalid;
    statefd = openat(dir, "state", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (statefd < 0 || secure_node(statefd, "ota_state.json", owner, 0) != 0 ||
        unique_json_keys(statefd, "ota_state.json", state_keys, 4) != 0)
        goto invalid;
    n = snprintf(statepath, sizeof(statepath), "%s/state", path);
    if (n < 0 || (size_t)n >= sizeof(statepath) ||
        ota_state_load(statepath, &state) != 0 ||
        (state.state != OTA_CONFIRMED && state.state != OTA_ROLLED_BACK))
        goto invalid;
    if (fstatat(dir, "last_operation.json", &st, AT_SYMLINK_NOFOLLOW) == 0) {
        if (!S_ISREG(st.st_mode) || st.st_uid != owner || (st.st_mode & 0022)) goto invalid;
    } else if (errno != ENOENT) goto invalid;
    if (ota_oplog_read(path, &oplog, &present) != 0 ||
        (present && (!ota_oplog_phase_is_terminal(oplog.phase) ||
                     unique_json_keys(dir, "last_operation.json", oplog_keys, 4) != 0 ||
                     strict_oplog(dir, &oplog) != 0))) goto invalid;
    len = readlinkat(dir, "current", current, sizeof(current) - 1);
    n = snprintf(expected, sizeof(expected), "releases/%s", state.confirmed_version);
    if (len < 0 || (size_t)len >= sizeof(current)-1 ||
        n < 0 || (size_t)n >= sizeof(expected)) goto invalid;
    current[len] = '\0';
    if (strcmp(current, expected) != 0) goto invalid;
    releases = openat(dir, "releases", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (releases < 0 || secure_node(releases, state.confirmed_version, owner, 1) != 0)
        goto invalid;
    release = openat(releases, state.confirmed_version,
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (release < 0 || secure_node(release, "smart_gateway", owner, 0) != 0)
        goto invalid;
    if (reason && size) snprintf(reason, size, "application OTA appears settled");
    rc = 0;
    goto out;
invalid:
    fail(reason, size, "application OTA unavailable, untrusted, busy or unfinished");
out:
    if (release >= 0) close(release);
    if (releases >= 0) close(releases);
    if (statefd >= 0) close(statefd);
    if (lock >= 0) { (void)flock(lock, LOCK_UN); close(lock); }
    if (dir >= 0) close(dir);
    return rc;
}
