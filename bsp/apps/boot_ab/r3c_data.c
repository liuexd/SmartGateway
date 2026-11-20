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

int r3c_data_mountinfo_matches_fstype(const char *text, const char *mountpoint,
                                      unsigned int dev_major, unsigned int dev_minor,
                                      const char *expected_fstype)
{
    char *copy, *line, *save = NULL;
    int found = 0, good = 0;
    const char *fst = (expected_fstype && expected_fstype[0]) ? expected_fstype : "ext4";
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
               strcmp(root, "/") == 0 && strcmp(type, fst) == 0 &&
               has_option(opts, "rw") && !has_option(opts, "ro");
    }
    free(copy);
    return found == 1 && good ? 0 : -1;
}

int r3c_data_mountinfo_matches(const char *text, const char *mountpoint,
                               unsigned int dev_major, unsigned int dev_minor)
{
    return r3c_data_mountinfo_matches_fstype(text, mountpoint, dev_major, dev_minor, "ext4");
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

static int trusted_parent_dirs(const char *path, uid_t owner)
{
    char buf[PATH_MAX], *slash;
    if (!safe_absolute(path)) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    slash = strrchr(buf, '/');
    if (!slash) return -1;
    if (slash == buf) {
        struct stat st;
        return (lstat("/", &st) == 0 && S_ISDIR(st.st_mode) &&
                st.st_uid == 0 && !(st.st_mode & 0022)) ? 0 : -1;
    }
    *slash = '\0';
    return trusted_dirs(buf, owner);
}

static int trusted_block_node(const char *path, uid_t owner, struct stat *out_st)
{
    struct stat lst;
    if (trusted_parent_dirs(path, owner) != 0) return -1;
    if (lstat(path, &lst) != 0 || !S_ISBLK(lst.st_mode)) return -1;
    if ((lst.st_uid != 0 && lst.st_uid != owner) || (lst.st_mode & 0002)) return -1;
    if (out_st) *out_st = lst;
    return 0;
}

static int read_mountinfo_at(const char *path, char **out)
{
    FILE *fp;
    char *line = NULL, *all = NULL;
    size_t cap = 0, used = 0;
    ssize_t n;
    fp = fopen((path && path[0]) ? path : "/proc/self/mountinfo", "re");
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

int r3c_data_inspect_contents(int dir, uid_t owner, uint64_t minimum,
                              char *reason, size_t size)
{
    struct stat schema, directory;
    struct statvfs vfs;
    char content[16];
    int fd = -1, rc = -1;
    ssize_t n;
    if (dir < 0 || minimum == 0 || fstat(dir, &directory) != 0 ||
        !S_ISDIR(directory.st_mode) || directory.st_uid != owner ||
        (directory.st_mode & 0022))
        return fail(reason, size, "untrusted DATA directory");
    if (fstatvfs(dir, &vfs) != 0 || (vfs.f_flag & ST_RDONLY) ||
        vfs.f_frsize == 0 ||
        vfs.f_bavail < (minimum / vfs.f_frsize +
                        (minimum % vfs.f_frsize != 0)) ||
        faccessat(dir, ".", W_OK, AT_EACCESS) != 0) {
        return fail(reason, size, "DATA read-only, inaccessible or low on space");
    }
    fd = openat(dir, ".sgw-data-schema", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &schema) != 0 || !S_ISREG(schema.st_mode) ||
        schema.st_uid != owner || (schema.st_mode & 0022) || schema.st_size != 9) {
        fail(reason, size, "missing or untrusted DATA schema"); goto out;
    }
    n = read(fd, content, sizeof(content));
    if (n != 9 || memcmp(content, "schema=1\n", 9) != 0) {
        fail(reason, size, "unsupported or invalid DATA schema"); goto out;
    }
    if (reason && size) snprintf(reason, size, "DATA contents prerequisites observed");
    rc = 0;
out:
    if (fd >= 0) close(fd);
    return rc;
}

int r3c_data_read_schema(int dir, uid_t owner, uint32_t *out_schema,
                         char *reason, size_t size)
{
    struct stat schema;
    char content[32];
    int fd = -1, rc = -1;
    ssize_t n;
    unsigned long long val = 0;
    char *end = NULL;

    if (out_schema) *out_schema = 0;
    if (dir < 0 || !out_schema) return fail(reason, size, "invalid DATA schema arguments");
    fd = openat(dir, ".sgw-data-schema", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &schema) != 0 || !S_ISREG(schema.st_mode) ||
        schema.st_uid != owner || (schema.st_mode & 0022) ||
        schema.st_size <= 7 || schema.st_size >= (off_t)sizeof(content)) {
        fail(reason, size, "missing or untrusted DATA schema");
        goto out;
    }
    n = read(fd, content, (size_t)schema.st_size);
    if (n != schema.st_size) {
        fail(reason, size, "failed reading DATA schema");
        goto out;
    }
    content[n] = '\0';
    if (strncmp(content, "schema=", 7) != 0) {
        fail(reason, size, "unsupported or invalid DATA schema");
        goto out;
    }
    errno = 0;
    val = strtoull(content + 7, &end, 10);
    if (errno || end == content + 7 || *end != '\n' || *(end + 1) != '\0' ||
        val == 0 || val > UINT32_MAX) {
        fail(reason, size, "unsupported or invalid DATA schema");
        goto out;
    }
    *out_schema = (uint32_t)val;
    rc = 0;
out:
    if (fd >= 0) close(fd);
    return rc;
}

int r3c_data_inspect(const r3c_data_policy_t *p, char *reason, size_t size)
{
    struct stat dev, root, mnt;
    char *mountinfo = NULL;
    int dir = -1, rc = -1;
    if (!p || !safe_absolute(p->mountpoint) || !safe_absolute(p->device) ||
        p->minimum_available_bytes == 0)
        return fail(reason, size, "invalid or incomplete policy");
    if (trusted_dirs(p->mountpoint, p->owner) != 0 ||
        trusted_block_node(p->device, p->owner, &dev) != 0 ||
        stat("/", &root) != 0)
        return fail(reason, size, "untrusted mount path or block device");
    dir = open(p->mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0 || fstat(dir, &mnt) != 0 || mnt.st_dev != dev.st_rdev ||
        mnt.st_dev == root.st_dev) {
        fail(reason, size, "mount device does not match or is root"); goto out;
    }
    if (read_mountinfo_at(NULL, &mountinfo) != 0 ||
        r3c_data_mountinfo_matches(mountinfo, p->mountpoint,
                                   major(dev.st_rdev), minor(dev.st_rdev)) != 0) {
        fail(reason, size, "mountinfo identity or mode invalid"); goto out;
    }
    if (r3c_data_inspect_contents(dir, p->owner, p->minimum_available_bytes,
                                  reason, size) != 0) goto out;
    if (reason && size) snprintf(reason, size, "DATA prerequisites observed");
    rc = 0;
out:
    free(mountinfo);
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

int r3c_app_ota_inspect_locked(const char *path, uid_t owner, int *lockfd,
                               char *reason, size_t size)
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
    if (!lockfd) return fail(reason, size, "missing lock output");
    *lockfd = -1;
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
    char rel_name[128];
    len = readlinkat(dir, "current", current, sizeof(current) - 1);
    n = snprintf(expected, sizeof(expected), "releases/%s", state.confirmed_version);
    if (len < 0 || (size_t)len >= sizeof(current)-1 ||
        n < 0 || (size_t)n >= sizeof(expected)) goto invalid;
    current[len] = '\0';
    snprintf(rel_name, sizeof(rel_name), "%s", state.confirmed_version);
    if (strcmp(current, expected) != 0) {
        if (state.confirmed_version[0] != 'v') {
            n = snprintf(expected, sizeof(expected), "releases/v%s", state.confirmed_version);
            if (n < 0 || (size_t)n >= sizeof(expected) || strcmp(current, expected) != 0)
                goto invalid;
            n = snprintf(rel_name, sizeof(rel_name), "v%s", state.confirmed_version);
            if (n < 0 || (size_t)n >= sizeof(rel_name)) goto invalid;
        } else {
            goto invalid;
        }
    }
    releases = openat(dir, "releases", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (releases < 0 || secure_node(releases, rel_name, owner, 1) != 0)
        goto invalid;
    release = openat(releases, rel_name,
                     O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (release < 0 || secure_node(release, "smart_gateway", owner, 0) != 0)
        goto invalid;
    *lockfd = lock;
    lock = -1;
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

int r3c_app_ota_inspect(const char *path, uid_t owner, char *reason, size_t size)
{
    int lock = -1;
    int rc = r3c_app_ota_inspect_locked(path, owner, &lock, reason, size);
    if (lock >= 0) { (void)flock(lock, LOCK_UN); close(lock); }
    return rc;
}


#include <sys/random.h>

static int read_u64_file(const char *path, uint64_t *out)
{
    char buf[64], *end = NULL;
    int fd;
    ssize_t n;
    unsigned long long v;
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return -1;
    buf[n] = '\0';
    errno = 0;
    v = strtoull(buf, &end, 10);
    if (errno || end == buf) return -1;
    while (*end == '\n' || *end == '\r' || *end == ' ' || *end == '\t') ++end;
    if (*end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

static int ranges_overlap(uint64_t a_start, uint64_t a_count,
                          uint64_t b_start, uint64_t b_count)
{
    uint64_t a_end, b_end;
    if (a_count == 0 || b_count == 0) return 0;
    if (a_start > UINT64_MAX - a_count || b_start > UINT64_MAX - b_count) return 1;
    a_end = a_start + a_count;
    b_end = b_start + b_count;
    return a_start < b_end && b_start < a_end;
}

static int resolve_disk_parent(const char *sysfs_root, unsigned int maj, unsigned int min,
                               char *disk_out, size_t cap)
{
    char linkpath[PATH_MAX], resolved[PATH_MAX], *slash, *last;
    int n = snprintf(linkpath, sizeof(linkpath), "%s/%u:%u", sysfs_root, maj, min);
    if (n <= 0 || (size_t)n >= sizeof(linkpath)) return -1;
    if (!realpath(linkpath, resolved)) return -1;
    /* Partition entries live under .../<disk>/<part>, disk entries under .../<disk>. */
    slash = strrchr(resolved, '/');
    if (!slash || slash == resolved) return -1;
    /* Check if 'partition' attribute exists in resolved dir; if so parent is disk. */
    {
        char partpath[PATH_MAX];
        struct stat pst;
        n = snprintf(partpath, sizeof(partpath), "%s/partition", resolved);
        if (n <= 0 || (size_t)n >= sizeof(partpath)) return -1;
        if (stat(partpath, &pst) == 0) {
            *slash = '\0';
            last = strrchr(resolved, '/');
            if (!last) return -1;
            if (snprintf(disk_out, cap, "%s", last + 1) >= (int)cap) return -1;
            return 1; /* is partition */
        }
    }
    if (snprintf(disk_out, cap, "%s", slash + 1) >= (int)cap) return -1;
    return 0; /* whole disk */
}

static int read_sysfs_geom(const char *sysfs_root, unsigned int maj, unsigned int min,
                           uint64_t *start, uint64_t *size, char *disk, size_t disk_cap,
                           int *is_part)
{
    char pstart[PATH_MAX], psize[PATH_MAX];
    int part_rc;
    part_rc = resolve_disk_parent(sysfs_root, maj, min, disk, disk_cap);
    if (part_rc < 0) return -1;
    *is_part = part_rc;
    if (snprintf(psize, sizeof(psize), "%s/%u:%u/size", sysfs_root, maj, min) >= (int)sizeof(psize))
        return -1;
    if (read_u64_file(psize, size) != 0 || *size == 0) return -1;
    if (part_rc == 1) {
        if (snprintf(pstart, sizeof(pstart), "%s/%u:%u/start", sysfs_root, maj, min) >= (int)sizeof(pstart))
            return -1;
        if (read_u64_file(pstart, start) != 0) return -1;
    } else {
        *start = 0;
    }
    return 0;
}

int r3c_data_check_slot_exclusion_sysfs(unsigned int dev_major, unsigned int dev_minor,
                                        const char *data_device,
                                        const char *whole_disk_device,
                                        const char *const *excluded_devices,
                                        size_t excluded_count,
                                        const r3c_sector_range_t *reserved_sectors,
                                        size_t reserved_count,
                                        const char *sysfs_block_root,
                                        uid_t owner,
                                        char *reason, size_t reason_size)
{
    const char *sysfs = (sysfs_block_root && sysfs_block_root[0]) ? sysfs_block_root : "/sys/dev/block";
    uint64_t d_start = 0, d_size = 0, w_start = 0, w_size = 0;
    char d_disk[128] = {0}, w_disk[128] = {0};
    int d_is_part = 0, w_is_part = 0;
    size_t i;

    if (!data_device || !safe_absolute(data_device) ||
        !excluded_devices || excluded_count == 0 ||
        !whole_disk_device || !safe_absolute(whole_disk_device))
        return fail(reason, reason_size, "invalid or incomplete slot exclusion policy");

    if (strcmp(data_device, whole_disk_device) == 0)
        return fail(reason, reason_size, "DATA device cannot be whole disk");

    /* Verify DATA sysfs geometry: must be a real partition starting past sector 0 */
    if (read_sysfs_geom(sysfs, dev_major, dev_minor, &d_start, &d_size,
                        d_disk, sizeof(d_disk), &d_is_part) != 0 ||
        !d_is_part || d_start == 0)
        return fail(reason, reason_size, "cannot prove DATA is a dedicated non-MBR partition");

    /* Verify whole disk identity cleanly (fail-closed if unknown/missing) */
    {
        struct stat wst;
        if (trusted_block_node(whole_disk_device, owner, &wst) != 0 ||
            (major(wst.st_rdev) == dev_major && minor(wst.st_rdev) == dev_minor) ||
            read_sysfs_geom(sysfs, major(wst.st_rdev), minor(wst.st_rdev),
                            &w_start, &w_size, w_disk, sizeof(w_disk), &w_is_part) != 0 ||
            w_is_part != 0)
            return fail(reason, reason_size, "whole disk identity unverified");
    }

    if (strcmp(d_disk, w_disk) == 0) {
        for (i = 0; i < reserved_count; ++i) {
            if (ranges_overlap(d_start, d_size,
                               reserved_sectors[i].start_sector,
                               reserved_sectors[i].sector_count)) {
                return fail(reason, reason_size, "DATA partition overlaps reserved boot/env sectors");
            }
        }
    }

    /* Every excluded system slot MUST resolve cleanly and be non-overlapping (fail-closed) */
    for (i = 0; i < excluded_count; ++i) {
        struct stat est;
        uint64_t e_start = 0, e_size = 0;
        char e_disk[128] = {0};
        int e_part = 0;
        const char *ex = excluded_devices[i];
        if (!ex || !safe_absolute(ex) || strcmp(data_device, ex) == 0)
            return fail(reason, reason_size, "DATA matches or invalid excluded system slot");
        if (trusted_block_node(ex, owner, &est) != 0)
            return fail(reason, reason_size, "excluded system slot device unverified or missing");
        if (major(est.st_rdev) == dev_major && minor(est.st_rdev) == dev_minor)
            return fail(reason, reason_size, "DATA block identity matches excluded system slot");
        if (read_sysfs_geom(sysfs, major(est.st_rdev), minor(est.st_rdev),
                            &e_start, &e_size, e_disk, sizeof(e_disk), &e_part) != 0)
            return fail(reason, reason_size, "cannot read excluded slot block geometry");
        if (strcmp(d_disk, e_disk) == 0) {
            if (!e_part)
                return fail(reason, reason_size, "excluded slot resolved to whole disk unexpectedly");
            if (ranges_overlap(d_start, d_size, e_start, e_size))
                return fail(reason, reason_size, "DATA partition overlaps excluded system slot sectors");
        }
    }
    return 0;
}

int r3c_data_verify_persistence(int dirfd, uid_t owner, char *reason, size_t reason_size)
{
    char name[64];
    unsigned char nonce[32], readbuf[32];
    int fd = -1, rfd = -1, rc = -1;
    ssize_t n;
    size_t i;
    struct stat st;

    if (dirfd < 0) return fail(reason, reason_size, "invalid DATA fd");
    if (getrandom(nonce, sizeof(nonce), GRND_NONBLOCK) != (ssize_t)sizeof(nonce))
        return fail(reason, reason_size, "entropy unavailable for DATA persistence probe");
    if (snprintf(name, sizeof(name), ".sgw-data-probe.%ld", (long)getpid()) >= (int)sizeof(name))
        return fail(reason, reason_size, "probe name overflow");
    (void)unlinkat(dirfd, name, 0);
    fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return fail(reason, reason_size, "cannot create DATA persistence probe");
    for (i = 0; i < sizeof(nonce); ) {
        n = write(fd, nonce + i, sizeof(nonce) - i);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto out;
        i += (size_t)n;
    }
    if (fsync(fd) != 0) { fail(reason, reason_size, "DATA probe fsync failed"); goto out; }
    if (close(fd) != 0) { fd = -1; fail(reason, reason_size, "DATA probe close failed"); goto out; }
    fd = -1;

    rfd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (rfd < 0 || fstat(rfd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != owner || (size_t)st.st_size != sizeof(nonce)) {
        fail(reason, reason_size, "DATA probe readback stat failed");
        goto out;
    }
    for (i = 0; i < sizeof(readbuf); ) {
        n = read(rfd, readbuf + i, sizeof(readbuf) - i);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto out;
        i += (size_t)n;
    }
    close(rfd); rfd = -1;
    if (memcmp(nonce, readbuf, sizeof(nonce)) != 0) {
        fail(reason, reason_size, "DATA probe readback mismatch");
        goto out;
    }
    if (unlinkat(dirfd, name, 0) != 0 || fsync(dirfd) != 0) {
        fail(reason, reason_size, "DATA probe cleanup fsync failed");
        goto out;
    }
    rc = 0;
out:
    if (fd >= 0) close(fd);
    if (rfd >= 0) close(rfd);
    if (rc != 0) { (void)unlinkat(dirfd, name, 0); (void)fsync(dirfd); }
    return rc;
}

int r3c_data_inspect_full(const r3c_data_full_policy_t *p, char *reason, size_t size)
{
    struct stat dev, root, mnt;
    char *mountinfo = NULL;
    int dir = -1, rc = -1;
    if (!p || !safe_absolute(p->mountpoint) || !safe_absolute(p->device) ||
        p->minimum_available_bytes == 0)
        return fail(reason, size, "invalid or incomplete policy");
    if (trusted_dirs(p->mountpoint, p->owner) != 0 ||
        trusted_block_node(p->device, p->owner, &dev) != 0 ||
        stat("/", &root) != 0)
        return fail(reason, size, "untrusted mount path or block device");
    if (r3c_data_check_slot_exclusion_sysfs(major(dev.st_rdev), minor(dev.st_rdev),
                                            p->device, p->whole_disk_device,
                                            p->excluded_devices, p->excluded_count,
                                            p->reserved_sectors, p->reserved_count,
                                            p->sysfs_block_root, p->owner,
                                            reason, size) != 0)
        return -1;
    dir = open(p->mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0 || fstat(dir, &mnt) != 0 || mnt.st_dev != dev.st_rdev ||
        mnt.st_dev == root.st_dev) {
        fail(reason, size, "mount device does not match or is root"); goto out;
    }
    if (read_mountinfo_at(p->mountinfo_path, &mountinfo) != 0 ||
        r3c_data_mountinfo_matches_fstype(mountinfo, p->mountpoint,
                                          major(dev.st_rdev), minor(dev.st_rdev),
                                          p->expected_fstype) != 0) {
        fail(reason, size, "mountinfo identity or mode invalid"); goto out;
    }
    if (r3c_data_inspect_contents(dir, p->owner, p->minimum_available_bytes,
                                  reason, size) != 0) goto out;
    if (p->verify_write_persistence &&
        r3c_data_verify_persistence(dir, p->owner, reason, size) != 0) goto out;
    if (reason && size) snprintf(reason, size, "DATA prerequisites observed");
    rc = 0;
out:
    free(mountinfo);
    if (dir >= 0) close(dir);
    return rc;
}
