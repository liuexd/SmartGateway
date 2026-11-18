#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "r3c_txn.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MAX_IMAGE_BYTES (2ULL * 1024 * 1024 * 1024)
#define MAX_MANIFEST_BYTES (64 * 1024)

static int fail(char *err, size_t len, const char *msg)
{
    if (err && len) snprintf(err, len, "%s", msg);
    return -1;
}

const char *r3c_txn_state_str(r3c_txn_state_t s)
{
    switch (s) {
    case R3C_TXN_VERIFIED: return "VERIFIED";
    case R3C_TXN_WRITTEN: return "WRITTEN";
    case R3C_TXN_PENDING: return "PENDING";
    case R3C_TXN_CONFIRMING: return "CONFIRMING";
    case R3C_TXN_CONFIRMED: return "CONFIRMED";
    default: return "INVALID";
    }
}

static int parse_state(const char *s, r3c_txn_state_t *out)
{
    if (strcmp(s, "VERIFIED") == 0) *out = R3C_TXN_VERIFIED;
    else if (strcmp(s, "WRITTEN") == 0) *out = R3C_TXN_WRITTEN;
    else if (strcmp(s, "PENDING") == 0) *out = R3C_TXN_PENDING;
    else if (strcmp(s, "CONFIRMING") == 0) *out = R3C_TXN_CONFIRMING;
    else if (strcmp(s, "CONFIRMED") == 0) *out = R3C_TXN_CONFIRMED;
    else return -1;
    return 0;
}

static int valid_hex64(const char *s)
{
    size_t i;
    int nonzero = 0;
    if (!s || strlen(s) != 64) return 0;
    for (i = 0; i < 64; ++i) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
        if (s[i] != '0') nonzero = 1;
    }
    return nonzero;
}

static int valid_boot_id(const char *s)
{
    size_t i;
    int nonzero = 0;
    if (!s || strlen(s) != 36) return 0;
    for (i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (s[i] != '-') return 0;
        } else {
            if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f'))) return 0;
            if (s[i] != '0') nonzero = 1;
        }
    }
    return nonzero;
}

static int safe_abs(const char *s)
{
    size_t n;
    if (!s || s[0] != '/' || s[1] == '\0') return 0;
    n = strlen(s);
    if (n >= PATH_MAX || s[n - 1] == '/' || strstr(s, "//") ||
        strstr(s, "/../") || strstr(s, "/./") || strpbrk(s, " \t\r\n\\"))
        return 0;
    if (n >= 3 && strcmp(s + n - 3, "/..") == 0) return 0;
    if (n >= 2 && strcmp(s + n - 2, "/.") == 0) return 0;
    return 1;
}

static int valid_libc_target(const char *name)
{
    size_t i, n = strlen(name);
    if (n < 9 || n >= 64 || strncmp(name, "libc-", 5) ||
        strcmp(name + n - 3, ".so")) return 0;
    for (i = 5; i < n - 3; ++i)
        if ((name[i] < '0' || name[i] > '9') && name[i] != '.') return 0;
    return 1;
}

static int open_libc_same_directory(const char *prefix)
{
    char libdir[PATH_MAX], target[64];
    struct stat st;
    ssize_t n;
    int dir, fd = -1;
    if (snprintf(libdir, sizeof(libdir), "%s/lib", prefix) >= (int)sizeof(libdir))
        return -1;
    dir = open(libdir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) return -1;
    if (fstatat(dir, "libc.so.6", &st, AT_SYMLINK_NOFOLLOW) == 0 && S_ISLNK(st.st_mode)) {
        n = readlinkat(dir, "libc.so.6", target, sizeof(target) - 1);
        if (n > 0 && (size_t)n < sizeof(target) - 1) {
            target[n] = '\0';
            if (valid_libc_target(target))
                fd = openat(dir, target, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        }
    }
    close(dir);
    return fd;
}

static int trusted_parent_dirs(const char *path, uid_t owner)
{
    char buf[PATH_MAX], *slash;
    struct stat st;
    size_t i, n;
    if (!safe_abs(path)) return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    slash = strrchr(buf, '/');
    if (!slash) return -1;
    if (slash == buf)
        return (lstat("/", &st) == 0 && S_ISDIR(st.st_mode) &&
                st.st_uid == 0 && !(st.st_mode & 0022)) ? 0 : -1;
    *slash = '\0';
    n = strlen(buf);
    if (lstat("/", &st) != 0 || st.st_uid != 0 || (st.st_mode & 0022)) return -1;
    for (i = 1; i <= n; ++i) {
        char saved;
        if (buf[i] != '/' && buf[i] != '\0') continue;
        saved = buf[i];
        buf[i] = '\0';
        if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode) ||
            (st.st_uid != 0 && st.st_uid != owner) || (st.st_mode & 0022))
            return -1;
        buf[i] = saved;
    }
    return 0;
}

static int secure_dir(int fd, uid_t owner)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == owner &&
           !(st.st_mode & 0077) ? 0 : -1;
}

int r3c_txn_acquire_lock(int txdir, uid_t owner, int *lockfd, char *err, size_t errlen)
{
    struct stat st;
    int fd;
    if (!lockfd) return fail(err, errlen, "null lockfd");
    *lockfd = -1;
    if (txdir < 0 || secure_dir(txdir, owner) != 0)
        return fail(err, errlen, "untrusted transaction directory");
    fd = openat(txdir, ".sgw-transaction.lock",
                O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (fd < 0) return fail(err, errlen, "cannot open transaction lock");
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != owner ||
        (st.st_mode & 0077) || st.st_nlink != 1 || flock(fd, LOCK_EX | LOCK_NB) != 0) {
        close(fd);
        return fail(err, errlen, "transaction lock busy or untrusted");
    }
    *lockfd = fd;
    return 0;
}

static int write_all(int fd, const char *s, size_t n)
{
    size_t used = 0;
    while (used < n) {
        ssize_t w = write(fd, s + used, n - used);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        used += (size_t)w;
    }
    return 0;
}

static int serialize_and_commit(int txdir, uid_t owner, const r3c_txn_record_t *rec,
                                char *err, size_t errlen)
{
    char name[80], tmp[96], body[1024];
    int fd = -1, n, created = 0, used = 0;
    if (!rec || !valid_hex64(rec->id) || !valid_hex64(rec->package_sha256) ||
        !valid_hex64(rec->image_sha256) || secure_dir(txdir, owner) != 0)
        return fail(err, errlen, "invalid transaction record for commit");

    n = snprintf(name, sizeof(name), "txn-%s", rec->id);
    if (n <= 0 || (size_t)n >= sizeof(name)) return fail(err, errlen, "name overflow");
    n = snprintf(tmp, sizeof(tmp), ".txn-%s.tmp.%ld", rec->id, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return fail(err, errlen, "tmp name overflow");

    n = snprintf(body, sizeof(body),
                 "SGW-TXN-1\nstate=%s\nid=%s\npackage=%s\nimage=%s\ntarget=%u\nlast_good=%u\n",
                 r3c_txn_state_str(rec->state), rec->id, rec->package_sha256,
                 rec->image_sha256, rec->target_slot, rec->last_good_slot);
    if (n <= 0 || (size_t)n >= sizeof(body)) return fail(err, errlen, "body overflow");
    used = n;
    if (rec->rootfs_manifest_sha256[0]) {
        n = snprintf(body + used, sizeof(body) - (size_t)used, "rootfs_manifest=%s\n",
                     rec->rootfs_manifest_sha256);
        if (n <= 0 || (size_t)(used + n) >= sizeof(body)) return fail(err, errlen, "body overflow");
        used += n;
    }
    if (rec->written_size > 0) {
        n = snprintf(body + used, sizeof(body) - (size_t)used, "written_size=%llu\nconsumed=%d\n",
                     (unsigned long long)rec->written_size, rec->consumed ? 1 : 0);
        if (n <= 0 || (size_t)(used + n) >= sizeof(body)) return fail(err, errlen, "body overflow");
        used += n;
    }
    if (rec->boot_id[0]) {
        n = snprintf(body + used, sizeof(body) - (size_t)used, "boot_id=%s\nbootcount=%lu\n",
                     rec->boot_id, rec->bootcount);
        if (n <= 0 || (size_t)(used + n) >= sizeof(body)) return fail(err, errlen, "body overflow");
        used += n;
    }

    (void)unlinkat(txdir, tmp, 0);
    fd = openat(txdir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return fail(err, errlen, "cannot create tmp transaction file");
    created = 1;
    if (write_all(fd, body, (size_t)used) != 0 || fsync(fd) != 0) {
        close(fd);
        (void)unlinkat(txdir, tmp, 0);
        (void)fsync(txdir);
        return fail(err, errlen, "transaction write/fsync failed");
    }
    if (close(fd) != 0) {
        (void)unlinkat(txdir, tmp, 0);
        (void)fsync(txdir);
        return fail(err, errlen, "transaction close failed");
    }
    if (renameat(txdir, tmp, txdir, name) != 0 || fsync(txdir) != 0) {
        if (created) (void)unlinkat(txdir, tmp, 0);
        return fail(err, errlen, "transaction atomic rename/fsync failed");
    }
    return 0;
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;
    if (!s || !s[0]) return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno || end == s || *end != '\0') return -1;
    *out = (uint64_t)v;
    return 0;
}

static int read_record_file(int txdir, const char *filename, const char *expected_id,
                            uid_t owner, r3c_txn_record_t *out, char *err, size_t errlen)
{
    char buf[1025];
    struct stat st;
    int fd;
    ssize_t n;
    char *line, *save = NULL;
    int line_no = 0;
    unsigned req = 0;

    if (!out || secure_dir(txdir, owner) != 0)
        return fail(err, errlen, "untrusted txdir");
    fd = openat(txdir, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return fail(err, errlen, "cannot open transaction record");
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != owner ||
        (st.st_mode & 0077) || st.st_nlink != 1 || st.st_size <= 0 || st.st_size > 1024) {
        close(fd);
        return fail(err, errlen, "untrusted transaction file metadata");
    }
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0 || n != st.st_size || memchr(buf, '\0', (size_t)n))
        return fail(err, errlen, "failed to read transaction record");
    buf[n] = '\0';

    memset(out, 0, sizeof(*out));
    for (line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *eq;
        if (line_no == 0) {
            if (strcmp(line, "SGW-TXN-1") != 0)
                return fail(err, errlen, "invalid transaction magic");
            line_no++;
            continue;
        }
        line_no++;
        eq = strchr(line, '=');
        if (!eq) return fail(err, errlen, "malformed transaction line");
        *eq = '\0';
        {
            const char *k = line, *v = eq + 1;
            if (strcmp(k, "state") == 0) {
                if ((req & 1u) || parse_state(v, &out->state) != 0)
                    return fail(err, errlen, "invalid transaction state");
                req |= 1u;
            } else if (strcmp(k, "id") == 0) {
                if ((req & 2u) || !valid_hex64(v))
                    return fail(err, errlen, "invalid transaction id");
                req |= 2u;
                snprintf(out->id, sizeof(out->id), "%s", v);
            } else if (strcmp(k, "package") == 0) {
                if ((req & 4u) || !valid_hex64(v))
                    return fail(err, errlen, "invalid package digest");
                req |= 4u;
                snprintf(out->package_sha256, sizeof(out->package_sha256), "%s", v);
            } else if (strcmp(k, "image") == 0) {
                if ((req & 8u) || !valid_hex64(v))
                    return fail(err, errlen, "invalid image digest");
                req |= 8u;
                snprintf(out->image_sha256, sizeof(out->image_sha256), "%s", v);
            } else if (strcmp(k, "rootfs_manifest") == 0) {
                if (!valid_hex64(v)) return fail(err, errlen, "invalid rootfs_manifest digest");
                snprintf(out->rootfs_manifest_sha256, sizeof(out->rootfs_manifest_sha256), "%s", v);
            } else if (strcmp(k, "target") == 0) {
                uint64_t val = 0;
                if ((req & 16u) || parse_u64(v, &val) != 0 || (val != 1 && val != 2))
                    return fail(err, errlen, "invalid target slot");
                req |= 16u;
                out->target_slot = (unsigned int)val;
            } else if (strcmp(k, "last_good") == 0) {
                uint64_t val = 0;
                if ((req & 32u) || parse_u64(v, &val) != 0 || (val != 1 && val != 2))
                    return fail(err, errlen, "invalid last_good slot");
                req |= 32u;
                out->last_good_slot = (unsigned int)val;
            } else if (strcmp(k, "written_size") == 0) {
                if (parse_u64(v, &out->written_size) != 0 || out->written_size == 0)
                    return fail(err, errlen, "invalid written_size");
            } else if (strcmp(k, "boot_id") == 0) {
                if (!valid_boot_id(v)) return fail(err, errlen, "invalid transaction boot_id");
                snprintf(out->boot_id, sizeof(out->boot_id), "%s", v);
            } else if (strcmp(k, "bootcount") == 0) {
                uint64_t val = 0;
                if (parse_u64(v, &val) != 0 || val == 0)
                    return fail(err, errlen, "invalid transaction bootcount");
                out->bootcount = (unsigned long)val;
            } else if (strcmp(k, "consumed") == 0) {
                if (strcmp(v, "1") == 0) out->consumed = 1;
                else if (strcmp(v, "0") == 0) out->consumed = 0;
                else return fail(err, errlen, "invalid consumed flag");
            } else {
                return fail(err, errlen, "unknown transaction field");
            }
        }
    }
    if (req != 63u || out->target_slot == out->last_good_slot)
        return fail(err, errlen, "incomplete transaction record");
    if (expected_id && strcmp(out->id, expected_id) != 0)
        return fail(err, errlen, "transaction id mismatch");
    return 0;
}

int r3c_txn_load_by_id(int txdir, const char *id, uid_t owner,
                       r3c_txn_record_t *out, char *err, size_t errlen)
{
    char name[80];
    if (!valid_hex64(id)) return fail(err, errlen, "invalid transaction id");
    if (snprintf(name, sizeof(name), "txn-%s", id) >= (int)sizeof(name))
        return fail(err, errlen, "name overflow");
    return read_record_file(txdir, name, id, owner, out, err, errlen);
}

static int matches_manifest_for_txn(const char *dname, const char *active_id)
{
    char expected[96];
    if (!active_id || !active_id[0]) return 0;
    snprintf(expected, sizeof(expected), "txn-%s.manifest", active_id);
    return strcmp(dname, expected) == 0;
}

int r3c_txn_load_active(int txdir, uid_t owner, r3c_txn_record_t *out,
                        char *err, size_t errlen)
{
    int dirfd = -1, active_count = 0;
    DIR *dir = NULL;
    struct dirent *entry;
    char manifest_names[4][260];
    int n_manifests = 0, i;

    if (!out || secure_dir(txdir, owner) != 0)
        return fail(err, errlen, "untrusted txdir");
    dirfd = openat(txdir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return fail(err, errlen, "cannot open txdir");
    dir = fdopendir(dirfd);
    if (!dir) { close(dirfd); return fail(err, errlen, "fdopendir failed"); }

    while ((entry = readdir(dir)) != NULL) {
        size_t dlen = strlen(entry->d_name);
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".sgw-transaction.lock") == 0)
            continue;
        if (strncmp(entry->d_name, ".txn-", 5) == 0 && strstr(entry->d_name, ".tmp.") != NULL)
            continue;
        if (dlen == 77 && strncmp(entry->d_name, "txn-", 4) == 0 &&
            strcmp(entry->d_name + 68, ".manifest") == 0) {
            if (n_manifests < 4) snprintf(manifest_names[n_manifests++], 260, "%s", entry->d_name);
            continue;
        }
        if (dlen != 68 || strncmp(entry->d_name, "txn-", 4) != 0 || !valid_hex64(entry->d_name + 4)) {
            closedir(dir);
            return fail(err, errlen, "unexpected file in transaction directory");
        }
        r3c_txn_record_t rec;
        if (read_record_file(txdir, entry->d_name, entry->d_name + 4, owner, &rec, err, errlen) != 0) {
            closedir(dir);
            return -1;
        }
        if (rec.consumed || rec.state == R3C_TXN_CONFIRMED)
            continue;
        active_count++;
        if (active_count > 1) {
            closedir(dir);
            return fail(err, errlen, "multiple active transactions present");
        }
        *out = rec;
    }
    closedir(dir);
    if (active_count != 1)
        return fail(err, errlen, "no active unconsumed transaction record");
    for (i = 0; i < n_manifests; ++i) {
        (void)matches_manifest_for_txn(manifest_names[i], out->id);
    }
    return 0;
}

int r3c_txn_digest_fd(int fd, uint64_t expected_len, char hex[65], uint64_t *actual_len)
{
    static const char table[] = "0123456789abcdef";
    unsigned char buf[16384], digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;
    struct stat st;
    EVP_MD_CTX *ctx = NULL;
    uint64_t target_len = expected_len, off = 0;
    int rc = -1;
    size_t i;

    if (fd < 0 || !hex || fstat(fd, &st) != 0) return -1;
    if (expected_len == 0) {
        if (!S_ISREG(st.st_mode) || st.st_size <= 0 || (uint64_t)st.st_size > MAX_IMAGE_BYTES)
            return -1;
        target_len = (uint64_t)st.st_size;
    } else if (expected_len > MAX_IMAGE_BYTES) {
        return -1;
    }

    ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto out;
    while (off < target_len) {
        size_t want = (size_t)(target_len - off);
        ssize_t n;
        if (want > sizeof(buf)) want = sizeof(buf);
        n = pread(fd, buf, want, (off_t)off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) goto out;
        off += (uint64_t)n;
    }
    if (EVP_DigestFinal_ex(ctx, digest, &dlen) != 1 || dlen != 32) goto out;
    for (i = 0; i < 32; ++i) {
        hex[i * 2] = table[digest[i] >> 4];
        hex[i * 2 + 1] = table[digest[i] & 15];
    }
    hex[64] = '\0';
    if (actual_len) *actual_len = target_len;
    rc = 0;
out:
    EVP_MD_CTX_free(ctx);
    return rc;
}

static int read_u64_sysfs(const char *path, uint64_t *out)
{
    char buf[64], *end = NULL;
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    ssize_t n;
    unsigned long long v;
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

static int ranges_overlap_u64(uint64_t a_start, uint64_t a_count,
                              uint64_t b_start, uint64_t b_count)
{
    uint64_t a_end, b_end;
    if (a_count == 0 || b_count == 0) return 0;
    if (a_start > UINT64_MAX - a_count || b_start > UINT64_MAX - b_count) return 1;
    a_end = a_start + a_count;
    b_end = b_start + b_count;
    return a_start < b_end && b_start < a_end;
}

static int read_blk_geom(const char *sysfs_root, unsigned int maj, unsigned int min,
                         uint64_t *start, uint64_t *size, char *disk, size_t dcap, int *is_part)
{
    char linkp[PATH_MAX], res[PATH_MAX], ppart[PATH_MAX], pstart[PATH_MAX], psize[PATH_MAX];
    char *sl, *parent_sl;
    struct stat pst;
    if (snprintf(linkp, sizeof(linkp), "%s/%u:%u", sysfs_root, maj, min) >= (int)sizeof(linkp))
        return -1;
    if (!realpath(linkp, res)) return -1;
    sl = strrchr(res, '/');
    if (!sl || sl == res) return -1;
    if (snprintf(psize, sizeof(psize), "%s/%u:%u/size", sysfs_root, maj, min) >= (int)sizeof(psize))
        return -1;
    if (read_u64_sysfs(psize, size) != 0 || *size == 0) return -1;
    if (snprintf(ppart, sizeof(ppart), "%s/partition", res) >= (int)sizeof(ppart))
        return -1;
    if (stat(ppart, &pst) == 0) {
        *is_part = 1;
        *sl = '\0';
        parent_sl = strrchr(res, '/');
        if (!parent_sl || snprintf(disk, dcap, "%s", parent_sl + 1) >= (int)dcap) return -1;
        if (snprintf(pstart, sizeof(pstart), "%s/%u:%u/start", sysfs_root, maj, min) >= (int)sizeof(pstart))
            return -1;
        if (read_u64_sysfs(pstart, start) != 0) return -1;
    } else {
        if (errno != ENOENT) return -1;
        *is_part = 0;
        *start = 0;
        if (snprintf(disk, dcap, "%s", sl + 1) >= (int)dcap) return -1;
    }
    return 0;
}

/* Reject stacked block devices unless their backing topology is independently
 * proven. Partitions inherit their parent's slaves; holders on either object
 * may indicate an indirect mount (dm-crypt/LVM/etc.). */
static int sysfs_relations_empty(const char *root, unsigned int maj, unsigned int min)
{
    char link[PATH_MAX], resolved[PATH_MAX], path[PATH_MAX], *slash;
    const char *relations[] = {"holders", "slaves"};
    int is_partition, i;
    struct stat st;
    if (snprintf(link, sizeof(link), "%s/%u:%u", root, maj, min) >= (int)sizeof(link) ||
        !realpath(link, resolved) ||
        snprintf(path, sizeof(path), "%s/partition", resolved) >= (int)sizeof(path))
        return -1;
    is_partition = stat(path, &st) == 0;
    if (!is_partition && errno != ENOENT) return -1;
    for (i = 0; i < (is_partition ? 3 : 2); ++i) {
        DIR *dir;
        struct dirent *entry;
        const char *rel = relations[is_partition ? (i == 2 ? 1 : 0) : i];
        char base[PATH_MAX];
        snprintf(base, sizeof(base), "%s", resolved);
        if (is_partition && (i == 1 || i == 2)) {
            slash = strrchr(base, '/');
            if (!slash || slash == base) return -1;
            *slash = '\0';
        }
        if (snprintf(path, sizeof(path), "%s/%s", base, rel) >= (int)sizeof(path))
            return -1;
        dir = opendir(path);
        if (!dir) return -1;
        errno = 0;
        while ((entry = readdir(dir)) != NULL) {
            if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) {
                closedir(dir);
                return -1;
            }
            errno = 0;
        }
        if (errno) { closedir(dir); return -1; }
        closedir(dir);
    }
    return 0;
}

typedef struct {
    dev_t dev;
    ino_t ino;
} r3c_ns_id_t;

static int scan_mountinfo_file(const char *path,
                               int ignore_missing,
                               const char *sysfs,
                               unsigned int maj, unsigned int min,
                               const char *t_disk,
                               uint64_t t_start, uint64_t t_size,
                               int have_t_geom,
                               int require_blk_geom,
                               char *err, size_t errlen)
{
    FILE *fp = fopen(path, "re");
    char line[4096];

    if (!fp) {
        if (ignore_missing && (errno == ENOENT || errno == ESRCH))
            return 0;
        return fail(err, errlen, "cannot read mountinfo (fail-closed)");
    }

    while (fgets(line, sizeof(line), fp)) {
        unsigned int lma = 0, lmi = 0;
        if (!strchr(line, '\n') || sscanf(line, "%*u %*u %u:%u ", &lma, &lmi) != 2) {
            fclose(fp);
            return fail(err, errlen, "malformed or truncated mountinfo");
        }
        if (lma == maj && lmi == min) {
            fclose(fp);
            return fail(err, errlen, "refusing write to currently mounted target block device");
        }
        if (have_t_geom && lma != 0) {
            uint64_t m_start = 0, m_size = 0;
            char m_disk[128] = {0};
            int m_is_part = 0;
            int known = read_blk_geom(sysfs, lma, lmi, &m_start, &m_size, m_disk, sizeof(m_disk), &m_is_part) == 0;
            if (require_blk_geom && (!known || sysfs_relations_empty(sysfs, lma, lmi) != 0)) {
                fclose(fp);
                return fail(err, errlen, "mounted block topology is unverified");
            }
            if (known && strcmp(t_disk, m_disk) == 0) {
                if (!m_is_part || ranges_overlap_u64(t_start, t_size, m_start, m_size)) {
                    fclose(fp);
                    return fail(err, errlen, "target partition overlaps mounted block topology");
                }
            }
        }
    }
    if (ferror(fp)) {
        int saved = errno;
        fclose(fp);
        if (ignore_missing && (saved == ENOENT || saved == ESRCH))
            return 0;
        return fail(err, errlen, "cannot read complete mountinfo");
    }
    fclose(fp);
    return 0;
}

static int ns_id_seen(const r3c_ns_id_t *seen, size_t count, dev_t dev, ino_t ino)
{
    for (size_t i = 0; i < count; ++i) {
        if (seen[i].dev == dev && seen[i].ino == ino)
            return 1;
    }
    return 0;
}

static int ns_id_push(r3c_ns_id_t **seen, size_t *count, size_t *cap, dev_t dev, ino_t ino)
{
    if (*count == *cap) {
        size_t next = (*cap == 0) ? 16 : (*cap * 2);
        r3c_ns_id_t *grown = realloc(*seen, next * sizeof(*grown));
        if (!grown)
            return -1;
        *seen = grown;
        *cap = next;
    }
    (*seen)[*count].dev = dev;
    (*seen)[*count].ino = ino;
    (*count)++;
    return 0;
}

static int is_ascii_pid_dir(const char *name)
{
    if (!name || !name[0])
        return 0;
    for (const char *p = name; *p; ++p) {
        if (!isdigit((unsigned char)*p))
            return 0;
    }
    return 1;
}

static int scan_proc_mount_namespaces(const char *proc_root,
                                      const char *sysfs,
                                      unsigned int maj, unsigned int min,
                                      const char *t_disk,
                                      uint64_t t_start, uint64_t t_size,
                                      int have_t_geom,
                                      int require_blk_geom,
                                      char *err, size_t errlen)
{
    char self_mi[PATH_MAX], self_ns[PATH_MAX];
    struct stat self_st;
    r3c_ns_id_t *seen = NULL;
    size_t n_seen = 0, cap_seen = 0;
    DIR *dp = NULL;
    struct dirent *ent;
    int rc = -1;

    if (snprintf(self_mi, sizeof(self_mi), "%s/self/mountinfo", proc_root) >= (int)sizeof(self_mi) ||
        snprintf(self_ns, sizeof(self_ns), "%s/self/ns/mnt", proc_root) >= (int)sizeof(self_ns))
        return fail(err, errlen, "proc mountinfo path too long");

    if (scan_mountinfo_file(self_mi, 0, sysfs, maj, min, t_disk, t_start, t_size,
                            have_t_geom, require_blk_geom, err, errlen) != 0)
        return -1;

    if (stat(self_ns, &self_st) == 0) {
        if (ns_id_push(&seen, &n_seen, &cap_seen, self_st.st_dev, self_st.st_ino) != 0) {
            fail(err, errlen, "out of memory tracking mount namespaces");
            goto out;
        }
    } else if (errno != ENOENT && errno != ENOTDIR) {
        fail(err, errlen, "cannot inspect self mount namespace (fail-closed)");
        goto out;
    }

    dp = opendir(proc_root);
    if (!dp) {
        fail(err, errlen, "cannot open proc for mount namespace scan");
        goto out;
    }
    for (;;) {
        char ns_path[PATH_MAX], mi_path[PATH_MAX];
        struct stat ns_st;
        errno = 0;
        ent = readdir(dp);
        if (!ent) {
            if (errno != 0) {
                fail(err, errlen, "error reading proc directory for mount namespaces");
                goto out;
            }
            break;
        }
        if (!is_ascii_pid_dir(ent->d_name))
            continue;
        if (snprintf(ns_path, sizeof(ns_path), "%s/%s/ns/mnt", proc_root, ent->d_name) >= (int)sizeof(ns_path) ||
            snprintf(mi_path, sizeof(mi_path), "%s/%s/mountinfo", proc_root, ent->d_name) >= (int)sizeof(mi_path))
            continue;
        if (stat(ns_path, &ns_st) != 0) {
            if (errno == ENOENT || errno == ESRCH)
                continue;
            fail(err, errlen, "cannot inspect foreign process mount namespace (fail-closed)");
            goto out;
        }
        if (ns_id_seen(seen, n_seen, ns_st.st_dev, ns_st.st_ino))
            continue;
        if (ns_id_push(&seen, &n_seen, &cap_seen, ns_st.st_dev, ns_st.st_ino) != 0) {
            fail(err, errlen, "out of memory tracking mount namespaces");
            goto out;
        }
        if (scan_mountinfo_file(mi_path, 1, sysfs, maj, min, t_disk, t_start, t_size,
                                have_t_geom, require_blk_geom, err, errlen) != 0)
            goto out;
    }
    rc = 0;

out:
    if (dp)
        closedir(dp);
    free(seen);
    return rc;
}

static int target_conflicts_with_mountinfo_or_slots_at(const char *mountinfo_path,
                                                       const char *proc_root,
                                                       const char *sysfs_root,
                                                       unsigned int maj, unsigned int min,
                                                       const char *running_dev,
                                                       const char *lg_dev,
                                                       int require_blk_geom,
                                                       char *err, size_t errlen)
{
    const char *sysfs = (sysfs_root && sysfs_root[0]) ? sysfs_root : "/sys/dev/block";
    const char *proc = (proc_root && proc_root[0]) ? proc_root : "/proc";
    char t_disk[128] = {0};
    uint64_t t_start = 0, t_size = 0;
    int t_is_part = 0, have_t_geom = 0;

    if (read_blk_geom(sysfs, maj, min, &t_start, &t_size, t_disk, sizeof(t_disk), &t_is_part) == 0) {
        have_t_geom = 1;
        if (!t_is_part || t_start == 0)
            return fail(err, errlen, "target must be a non-MBR partition, not whole disk");
        if (ranges_overlap_u64(t_start, t_size, 1792ULL, 32ULL))
            return fail(err, errlen, "target partition overlaps reserved U-Boot env sectors");
        if (require_blk_geom && sysfs_relations_empty(sysfs, maj, min) != 0)
            return fail(err, errlen, "target stacked device topology is unverified");
    } else if (require_blk_geom) {
        return fail(err, errlen, "cannot verify target block topology in sysfs");
    }

    if (mountinfo_path && mountinfo_path[0]) {
        if (scan_mountinfo_file(mountinfo_path, 0, sysfs, maj, min, t_disk, t_start, t_size,
                                have_t_geom, require_blk_geom, err, errlen) != 0)
            return -1;
    } else {
        if (scan_proc_mount_namespaces(proc, sysfs, maj, min, t_disk, t_start, t_size,
                                       have_t_geom, require_blk_geom, err, errlen) != 0)
            return -1;
    }

    /* Also check running_dev and last_good_dev sysfs range exclusion if present */
    if (have_t_geom) {
        const char *slots[2] = { running_dev, lg_dev };
        for (int s = 0; s < 2; ++s) {
            struct stat sst;
            uint64_t s_start = 0, s_size = 0;
            char s_disk[128] = {0};
            int s_part = 0;
            if (!slots[s]) {
                if (require_blk_geom) return fail(err, errlen, "missing protected slot identity");
                continue;
            }
            if (stat(slots[s], &sst) == 0 && S_ISBLK(sst.st_mode)) {
                if (major(sst.st_rdev) == maj && minor(sst.st_rdev) == min)
                    return fail(err, errlen, "target block identity matches active/last_good slot");
                int known = read_blk_geom(sysfs, major(sst.st_rdev), minor(sst.st_rdev),
                                          &s_start, &s_size, s_disk, sizeof(s_disk), &s_part) == 0;
                if (require_blk_geom && (!known || sysfs_relations_empty(sysfs, major(sst.st_rdev),
                                                                         minor(sst.st_rdev)) != 0))
                    return fail(err, errlen, "protected slot topology is unverified");
                if (known &&
                    strcmp(t_disk, s_disk) == 0 &&
                    (!s_part || ranges_overlap_u64(t_start, t_size, s_start, s_size))) {
                    return fail(err, errlen, "target partition sectors overlap active/last_good slot");
                }
            } else if (require_blk_geom)
                return fail(err, errlen, "protected slot device missing or not a block device");
        }
    }
    return 0;
}

static int target_conflicts_with_mountinfo_or_slots(const char *mountinfo_path,
                                                    const char *sysfs_root,
                                                    unsigned int maj, unsigned int min,
                                                    const char *running_dev,
                                                    const char *lg_dev,
                                                    int require_blk_geom,
                                                    char *err, size_t errlen)
{
    return target_conflicts_with_mountinfo_or_slots_at(
        mountinfo_path, "/proc", sysfs_root, maj, min,
        running_dev, lg_dev, require_blk_geom, err, errlen);
}

int r3c_txn_write_and_verify_slot(int txdir, const char *id, uid_t owner,
                                  int imagefd,
                                  const fw_env_backend_cfg_t *fw_cfg,
                                  const ab_health_cfg_t *hcfg,
                                  const r3c_slot_write_opts_t *opts,
                                  char *err, size_t errlen)
{
    int lockfd = -1, slotfd = -1, rc = -1;
    r3c_txn_record_t rec;
    ab_slot_t running_slot = AB_SLOT_NONE;
    ab_env_t env;
    char src_hex[65], rb_hex[65];
    uint64_t src_len = 0, rb_len = 0, off = 0, cap_bytes = 0;
    unsigned char buf[16384];
    const char *opt_mi_path = (opts && opts->mountinfo_path && opts->mountinfo_path[0]) ? opts->mountinfo_path : NULL;
    const char *mi_path = opt_mi_path ? opt_mi_path : "/proc/self/mountinfo";
    const char *sysfs_root = (opts && opts->sysfs_block_root) ? opts->sysfs_block_root : "/sys/dev/block";
    int allow_reg = (opts && opts->allow_regular_file_target) ? 1 : 0;
    int skip_blk = (opts && opts->allow_host_slot_sim) ? 1 : 0;
    const char *target_path = NULL, *running_dev = NULL, *lg_dev = NULL;
    struct stat tst, opened_st, rst, lst_run, lst_lg;
    int had_pre_stat = 0;

    if (imagefd < 0 || !fw_cfg || !hcfg)
        return fail(err, errlen, "invalid arguments for slot write");
    if (!opts && geteuid() != 0)
        return fail(err, errlen, "production slot write requires root privilege");
    if (r3c_txn_acquire_lock(txdir, owner, &lockfd, err, errlen) != 0) return -1;
    if (r3c_txn_load_by_id(txdir, id, owner, &rec, err, errlen) != 0) goto out;
    if (rec.state != R3C_TXN_VERIFIED || rec.consumed) {
        fail(err, errlen, "transaction not in VERIFIED state");
        goto out;
    }

    /* Independently verify running slot & U-Boot env BEFORE opening or writing ANY target */
    if (ab_detect_running_slot_full(hcfg, mi_path, skip_blk, &running_slot, err, errlen) != 0)
        goto out;
    if (fw_env_backend_load(fw_cfg, &env, err, errlen) != 0) goto out;
    if (ab_env_validate(&env, err, errlen) != 0) goto out;
    if (env.ab_state == AB_STATE_TRIAL || env.upgrade_available != 0) {
        fail(err, errlen, "refusing slot write while A/B trial upgrade is active");
        goto out;
    }
    if ((ab_slot_t)rec.target_slot == running_slot ||
        (ab_slot_t)rec.target_slot != ab_slot_other(running_slot) ||
        (ab_slot_t)rec.last_good_slot != running_slot ||
        (ab_slot_t)rec.target_slot == env.active_slot ||
        (ab_slot_t)rec.target_slot == env.last_good_slot) {
        fail(err, errlen, "transaction target slot conflicts with running or last_good slot");
        goto out;
    }

    running_dev = ab_slot_device(running_slot);
    lg_dev = ab_slot_device(env.last_good_slot);
    target_path = (opts && opts->target_dev_override) ?
        opts->target_dev_override : ab_slot_device((ab_slot_t)rec.target_slot);
    if (!target_path || !safe_abs(target_path) || trusted_parent_dirs(target_path, owner) != 0) {
        fail(err, errlen, "untrusted target slot path");
        goto out;
    }
    if ((running_dev && strcmp(target_path, running_dev) == 0) ||
        (lg_dev && strcmp(target_path, lg_dev) == 0)) {
        fail(err, errlen, "target slot path equals active running/last_good slot");
        goto out;
    }
    if (r3c_txn_digest_fd(imagefd, 0, src_hex, &src_len) != 0 ||
        strcmp(src_hex, rec.image_sha256) != 0) {
        fail(err, errlen, "verified image digest mismatch before slot write");
        goto out;
    }
    if (stat("/", &rst) != 0) {
        fail(err, errlen, "cannot stat root '/'");
        goto out;
    }

    if (lstat(target_path, &tst) != 0) {
        if (!allow_reg || errno != ENOENT) {
            fail(err, errlen, "target slot device missing or inaccessible");
            goto out;
        }
    } else {
        had_pre_stat = 1;
        if (S_ISLNK(tst.st_mode)) {
            fail(err, errlen, "target slot cannot be symlink");
            goto out;
        }
        if (S_ISBLK(tst.st_mode)) {
            if (tst.st_rdev == rst.st_dev) {
                fail(err, errlen, "target block device matches mounted root");
                goto out;
            }
            if (running_dev && stat(running_dev, &lst_run) == 0 && S_ISBLK(lst_run.st_mode) &&
                tst.st_rdev == lst_run.st_rdev) {
                fail(err, errlen, "target block device identity matches running slot");
                goto out;
            }
            if (lg_dev && stat(lg_dev, &lst_lg) == 0 && S_ISBLK(lst_lg.st_mode) &&
                tst.st_rdev == lst_lg.st_rdev) {
                fail(err, errlen, "target block device identity matches last_good slot");
                goto out;
            }
            if (target_conflicts_with_mountinfo_or_slots(
                    opt_mi_path, sysfs_root, major(tst.st_rdev), minor(tst.st_rdev),
                    running_dev, lg_dev, !allow_reg, err, errlen) != 0) {
                goto out;
            }
        } else if (!allow_reg || !S_ISREG(tst.st_mode)) {
            fail(err, errlen, "target slot is not a block device");
            goto out;
        } else {
            if (tst.st_size > 0 && (uint64_t)tst.st_size < src_len) {
                fail(err, errlen, "target image container smaller than verified payload");
                goto out;
            }
        }
    }

    slotfd = open(target_path,
                  allow_reg ? (O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW) : (O_RDWR | O_CLOEXEC | O_NOFOLLOW),
                  0600);
    if (slotfd < 0 || fstat(slotfd, &opened_st) != 0) {
        fail(err, errlen, "cannot open verified target slot");
        goto out;
    }
    /* Cross-verify opened fd identity against pre-open lstat to block TOCTOU device swap */
    if (had_pre_stat) {
        if ((opened_st.st_mode & S_IFMT) != (tst.st_mode & S_IFMT) ||
            opened_st.st_dev != tst.st_dev || opened_st.st_ino != tst.st_ino ||
            (S_ISBLK(opened_st.st_mode) && opened_st.st_rdev != tst.st_rdev)) {
            fail(err, errlen, "target slot identity changed between lstat and open");
            goto out;
        }
    }
    if (S_ISBLK(opened_st.st_mode)) {
        if (target_conflicts_with_mountinfo_or_slots(
                opt_mi_path, sysfs_root, major(opened_st.st_rdev), minor(opened_st.st_rdev),
                running_dev, lg_dev, !allow_reg, err, errlen) != 0) {
            goto out;
        }
        if (ioctl(slotfd, BLKGETSIZE64, &cap_bytes) != 0 || cap_bytes < src_len) {
            fail(err, errlen, "target block device capacity insufficient");
            goto out;
        }
    } else if (!allow_reg || !S_ISREG(opened_st.st_mode)) {
        fail(err, errlen, "opened target descriptor is not block device");
        goto out;
    } else if (had_pre_stat && opened_st.st_size > 0 && (uint64_t)opened_st.st_size < src_len) {
        fail(err, errlen, "opened target container smaller than verified payload");
        goto out;
    }

    while (off < src_len) {
        size_t want = (size_t)(src_len - off);
        ssize_t r, w, done = 0;
        if (want > sizeof(buf)) want = sizeof(buf);
        r = pread(imagefd, buf, want, (off_t)off);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) { fail(err, errlen, "image read error"); goto out; }
        while (done < r) {
            w = pwrite(slotfd, buf + done, (size_t)(r - done), (off_t)(off + (uint64_t)done));
            if (w < 0 && errno == EINTR) continue;
            if (w <= 0) { fail(err, errlen, "slot write error"); goto out; }
            done += w;
        }
        off += (uint64_t)r;
    }
    if (fsync(slotfd) != 0) {
        fail(err, errlen, "slot fsync failed");
        goto out;
    }
    if (r3c_txn_digest_fd(slotfd, src_len, rb_hex, &rb_len) != 0 ||
        rb_len != src_len || strcmp(rb_hex, rec.image_sha256) != 0) {
        fail(err, errlen, "slot readback digest mismatch");
        goto out;
    }
    rec.state = R3C_TXN_WRITTEN;
    rec.written_size = src_len;
    if (serialize_and_commit(txdir, owner, &rec, err, errlen) != 0) goto out;
    rc = 0;
out:
    if (slotfd >= 0) close(slotfd);
    if (lockfd >= 0) { (void)flock(lockfd, LOCK_UN); close(lockfd); }
    return rc;
}

int r3c_txn_verify_installed_rootfs(int txdir, const r3c_txn_record_t *rec, uid_t owner,
                                    const char *sysroot_prefix, int root_is_rw,
                                    int raw_slotfd,
                                    char *err, size_t errlen)
{
    char mname[96], mhash[65], *mbuf = NULL, *line, *save = NULL;
    uint64_t mlen = 0;
    struct stat mst;
    int mfd = -1, rc = -1;
    ssize_t n;
    const char *prefix = sysroot_prefix ? sysroot_prefix : "";

    if (!rec) return fail(err, errlen, "null transaction record");
    if (!rec->rootfs_manifest_sha256[0]) {
        /* SGW-SYSTEM-1 legacy bundle has no file-level manifest: only valid for read-only mounted rootfs */
        char slot_hex[65];
        uint64_t slot_len = 0;
        if (root_is_rw)
            return fail(err, errlen, "rw-mounted rootfs requires signed rootfs_manifest");
        if (raw_slotfd < 0 ||
            r3c_txn_digest_fd(raw_slotfd, rec->written_size, slot_hex, &slot_len) != 0 ||
            slot_len != rec->written_size || strcmp(slot_hex, rec->image_sha256) != 0)
            return fail(err, errlen, "ro rootfs raw slot digest mismatch");
        return 0;
    }

    if (snprintf(mname, sizeof(mname), "txn-%s.manifest", rec->id) >= (int)sizeof(mname))
        return fail(err, errlen, "manifest name overflow");
    mfd = openat(txdir, mname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (mfd < 0 || fstat(mfd, &mst) != 0 || !S_ISREG(mst.st_mode) || mst.st_uid != owner ||
        (mst.st_mode & 0077) || mst.st_size <= 0 || mst.st_size > MAX_MANIFEST_BYTES) {
        fail(err, errlen, "missing or untrusted txn rootfs.manifest");
        goto out;
    }
    if (r3c_txn_digest_fd(mfd, 0, mhash, &mlen) != 0 ||
        strcmp(mhash, rec->rootfs_manifest_sha256) != 0) {
        fail(err, errlen, "txn rootfs.manifest digest mismatch");
        goto out;
    }
    mbuf = malloc((size_t)mlen + 1);
    if (!mbuf) { fail(err, errlen, "oom"); goto out; }
    n = pread(mfd, mbuf, (size_t)mlen, 0);
    if (n != (ssize_t)mlen || memchr(mbuf, '\0', (size_t)n)) {
        fail(err, errlen, "cannot read txn rootfs.manifest");
        goto out;
    }
    mbuf[n] = '\0';

    static const char *const MANDATORY_PATHS[] = {
        "/sbin/init", "/etc/inittab", "/etc/fstab", "/etc/fw_env.config",
        "/sbin/fw_printenv", "/usr/bin/sgw_bootctl", "/usr/bin/sgw_watchdog_guard",
        "/usr/bin/service_supervisor", "/usr/bin/smart_gateway", "/usr/bin/ota_agent",
        "/etc/init.d/S15sgw_ab_guard", "/etc/init.d/S99smartgateway",
        "/boot/zImage", "/boot/100ask_imx6ull-14x14.dtb", "/lib/libc.so.6"
    };
    const size_t MANDATORY_N = sizeof(MANDATORY_PATHS) / sizeof(MANDATORY_PATHS[0]);
    unsigned int mandatory_mask = 0;
    char seen_paths[256][256];
    int seen_count = 0;

    for (line = strtok_r(mbuf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *sp = strchr(line, ' ');
        char full[PATH_MAX], fhex[65];
        uint64_t flen = 0;
        struct stat fst;
        int ffd;
        if (!sp) { fail(err, errlen, "malformed rootfs.manifest line"); goto out; }
        *sp = '\0';
        if (!safe_abs(line) || !valid_hex64(sp + 1)) {
            fail(err, errlen, "invalid rootfs.manifest entry");
            goto out;
        }
        if (snprintf(full, sizeof(full), "%s%s", prefix, line) >= (int)sizeof(full)) {
            fail(err, errlen, "sysroot file path overflow");
            goto out;
        }
        ffd = open(full, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (ffd < 0 && errno == ELOOP && strcmp(line, "/lib/libc.so.6") == 0)
            ffd = open_libc_same_directory(prefix);
        if (ffd < 0) { fail(err, errlen, "missing signed system file in running rootfs"); goto out; }
        if (fstat(ffd, &fst) != 0 || !S_ISREG(fst.st_mode) ||
            r3c_txn_digest_fd(ffd, 0, fhex, &flen) != 0 ||
            strcmp(fhex, sp + 1) != 0) {
            close(ffd);
            fail(err, errlen, "running system file digest mismatch against signed rootfs_manifest");
            goto out;
        }
        close(ffd);
        if (seen_count >= 256) { fail(err, errlen, "too many manifest entries"); goto out; }
        for (int s = 0; s < seen_count; ++s) {
            if (strcmp(seen_paths[s], line) == 0) {
                fail(err, errlen, "duplicate path in txn rootfs.manifest");
                goto out;
            }
        }
        snprintf(seen_paths[seen_count++], 256, "%s", line);
        for (size_t m = 0; m < MANDATORY_N; ++m) {
            if (strcmp(line, MANDATORY_PATHS[m]) == 0) mandatory_mask |= (1u << m);
        }
    }
    if (mandatory_mask != ((1u << MANDATORY_N) - 1u)) {
        fail(err, errlen, "txn rootfs.manifest missing mandatory protected system file");
        goto out;
    }
    rc = 0;
out:
    free(mbuf);
    if (mfd >= 0) close(mfd);
    return rc;
}

int r3c_txn_stage_pending(int txdir, const char *id, uid_t owner,
                          const fw_env_backend_cfg_t *fw_cfg,
                          const ab_health_cfg_t *hcfg,
                          ab_slot_t running_slot,
                          char *err, size_t errlen)
{
    int lockfd = -1, rc = -1;
    r3c_txn_record_t rec;
    ab_env_t env, verify_env;
    (void)hcfg;

    if (!fw_cfg) return fail(err, errlen, "null fw_cfg");
    if (r3c_txn_acquire_lock(txdir, owner, &lockfd, err, errlen) != 0) return -1;
    if (r3c_txn_load_by_id(txdir, id, owner, &rec, err, errlen) != 0) goto out;
    if (rec.state != R3C_TXN_WRITTEN || rec.consumed || rec.written_size == 0) {
        fail(err, errlen, "transaction not in WRITTEN state");
        goto out;
    }
    if ((ab_slot_t)rec.last_good_slot != running_slot) {
        fail(err, errlen, "running slot disagrees with transaction last_good_slot");
        goto out;
    }
    if (fw_env_backend_load(fw_cfg, &env, err, errlen) != 0) goto out;
    if (ab_stage_trial(&env, (ab_slot_t)rec.target_slot, running_slot, err, errlen) != 0) goto out;
    if (fw_env_backend_save(fw_cfg, &env, err, errlen) != 0) goto out;
    if (fw_env_backend_load(fw_cfg, &verify_env, err, errlen) != 0) goto out;
    if (verify_env.ab_state != AB_STATE_TRIAL || verify_env.upgrade_available != 1 ||
        verify_env.pending_slot != (ab_slot_t)rec.target_slot ||
        verify_env.last_good_slot != running_slot) {
        fail(err, errlen, "fw_env stage readback verification failed");
        goto out;
    }
    rec.state = R3C_TXN_PENDING;
    if (serialize_and_commit(txdir, owner, &rec, err, errlen) != 0) goto out;
    rc = 0;
out:
    if (lockfd >= 0) { (void)flock(lockfd, LOCK_UN); close(lockfd); }
    return rc;
}

int r3c_txn_begin_confirm(int txdir, const char *id, uid_t owner,
                          ab_slot_t running_slot, ab_slot_t last_good_slot,
                          const char *boot_id, unsigned long bootcount,
                          const char *sysroot_prefix, int root_is_rw, int slotfd,
                          char *err, size_t errlen)
{
    r3c_txn_record_t rec;

    if (!valid_boot_id(boot_id) || bootcount == 0)
        return fail(err, errlen, "invalid boot_id or bootcount");
    if (r3c_txn_load_by_id(txdir, id, owner, &rec, err, errlen) != 0) return -1;
    if (rec.consumed || (rec.state != R3C_TXN_PENDING && rec.state != R3C_TXN_CONFIRMING))
        return fail(err, errlen, "transaction not pending/confirmable");
    if ((ab_slot_t)rec.target_slot != running_slot ||
        (ab_slot_t)rec.last_good_slot != last_good_slot ||
        rec.written_size == 0)
        return fail(err, errlen, "transaction slot or size mismatch");
    if (r3c_txn_verify_installed_rootfs(txdir, &rec, owner, sysroot_prefix,
                                        root_is_rw, slotfd, err, errlen) != 0)
        return -1;

    rec.state = R3C_TXN_CONFIRMING;
    snprintf(rec.boot_id, sizeof(rec.boot_id), "%s", boot_id);
    rec.bootcount = bootcount;
    return serialize_and_commit(txdir, owner, &rec, err, errlen);
}

int r3c_txn_finish_confirm(int txdir, const char *id, uid_t owner,
                           const char *boot_id, unsigned long bootcount,
                           char *err, size_t errlen)
{
    r3c_txn_record_t rec;
    if (!valid_boot_id(boot_id) || bootcount == 0)
        return fail(err, errlen, "invalid boot_id or bootcount");
    if (r3c_txn_load_by_id(txdir, id, owner, &rec, err, errlen) != 0) return -1;
    if (rec.consumed || rec.state != R3C_TXN_CONFIRMING)
        return fail(err, errlen, "transaction not in CONFIRMING state");
    if (strcmp(rec.boot_id, boot_id) != 0 || rec.bootcount != bootcount)
        return fail(err, errlen, "transaction boot binding changed before commit");
    rec.state = R3C_TXN_CONFIRMED;
    rec.consumed = 1;
    return serialize_and_commit(txdir, owner, &rec, err, errlen);
}

int r3c_txn_reconcile_confirm(int txdir, const char *id, uid_t owner,
                              ab_slot_t running_slot,
                              const char *sysroot_prefix, int root_is_rw, int slotfd,
                              char *err, size_t errlen)
{
    r3c_txn_record_t rec;

    if (r3c_txn_load_by_id(txdir, id, owner, &rec, err, errlen) != 0) return -1;
    if (rec.consumed || rec.state != R3C_TXN_CONFIRMING)
        return fail(err, errlen, "transaction not in recoverable CONFIRMING state");
    if ((ab_slot_t)rec.target_slot != running_slot ||
        !valid_boot_id(rec.boot_id) || rec.bootcount == 0 || rec.written_size == 0)
        return fail(err, errlen, "CONFIRMING transaction missing verified binding");
    if (r3c_txn_verify_installed_rootfs(txdir, &rec, owner, sysroot_prefix,
                                        root_is_rw, slotfd, err, errlen) != 0)
        return -1;
    rec.state = R3C_TXN_CONFIRMED;
    rec.consumed = 1;
    return serialize_and_commit(txdir, owner, &rec, err, errlen);
}
