#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "r3c_txn.h"

#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAX_IMAGE_BYTES (2ULL * 1024 * 1024 * 1024)

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
    int fd = -1, n, created = 0;
    if (!rec || !valid_hex64(rec->id) || !valid_hex64(rec->package_sha256) ||
        !valid_hex64(rec->image_sha256) || secure_dir(txdir, owner) != 0)
        return fail(err, errlen, "invalid transaction record for commit");

    n = snprintf(name, sizeof(name), "txn-%s", rec->id);
    if (n <= 0 || (size_t)n >= sizeof(name)) return fail(err, errlen, "name overflow");
    n = snprintf(tmp, sizeof(tmp), ".txn-%s.tmp.%ld", rec->id, (long)getpid());
    if (n <= 0 || (size_t)n >= sizeof(tmp)) return fail(err, errlen, "tmp name overflow");

    if (rec->boot_id[0]) {
        n = snprintf(body, sizeof(body),
                     "SGW-TXN-1\nstate=%s\nid=%s\npackage=%s\nimage=%s\ntarget=%u\nlast_good=%u\n"
                     "written_size=%llu\nboot_id=%s\nbootcount=%lu\nconsumed=%d\n",
                     r3c_txn_state_str(rec->state), rec->id, rec->package_sha256,
                     rec->image_sha256, rec->target_slot, rec->last_good_slot,
                     (unsigned long long)rec->written_size, rec->boot_id,
                     rec->bootcount, rec->consumed ? 1 : 0);
    } else if (rec->written_size > 0) {
        n = snprintf(body, sizeof(body),
                     "SGW-TXN-1\nstate=%s\nid=%s\npackage=%s\nimage=%s\ntarget=%u\nlast_good=%u\n"
                     "written_size=%llu\nconsumed=%d\n",
                     r3c_txn_state_str(rec->state), rec->id, rec->package_sha256,
                     rec->image_sha256, rec->target_slot, rec->last_good_slot,
                     (unsigned long long)rec->written_size, rec->consumed ? 1 : 0);
    } else {
        n = snprintf(body, sizeof(body),
                     "SGW-TXN-1\nstate=%s\nid=%s\npackage=%s\nimage=%s\ntarget=%u\nlast_good=%u\n",
                     r3c_txn_state_str(rec->state), rec->id, rec->package_sha256,
                     rec->image_sha256, rec->target_slot, rec->last_good_slot);
    }
    if (n <= 0 || (size_t)n >= sizeof(body)) return fail(err, errlen, "body overflow");

    (void)unlinkat(txdir, tmp, 0);
    fd = openat(txdir, tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return fail(err, errlen, "cannot create tmp transaction file");
    created = 1;
    if (write_all(fd, body, (size_t)n) != 0 || fsync(fd) != 0) {
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

int r3c_txn_load_active(int txdir, uid_t owner, r3c_txn_record_t *out,
                        char *err, size_t errlen)
{
    int dirfd = -1, active_count = 0;
    DIR *dir = NULL;
    struct dirent *entry;

    if (!out || secure_dir(txdir, owner) != 0)
        return fail(err, errlen, "untrusted txdir");
    dirfd = openat(txdir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dirfd < 0) return fail(err, errlen, "cannot open txdir");
    dir = fdopendir(dirfd);
    if (!dir) { close(dirfd); return fail(err, errlen, "fdopendir failed"); }

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, ".sgw-transaction.lock") == 0)
            continue;
        if (strncmp(entry->d_name, ".txn-", 5) == 0 && strstr(entry->d_name, ".tmp.") != NULL) {
            /* Unfinished tmp artifact indicates interrupted crash or concurrent writer; ignore/reject safely */
            continue;
        }
        if (strncmp(entry->d_name, "txn-", 4) != 0 || !valid_hex64(entry->d_name + 4)) {
            closedir(dir);
            return fail(err, errlen, "unexpected file in transaction directory");
        }
        r3c_txn_record_t rec;
        if (read_record_file(txdir, entry->d_name, entry->d_name + 4, owner, &rec, err, errlen) != 0) {
            closedir(dir);
            return -1;
        }
        if (rec.consumed || rec.state == R3C_TXN_CONFIRMED) {
            continue; /* Consumed/completed history cannot be re-activated */
        }
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

int r3c_txn_write_and_verify_slot(int txdir, const char *id, uid_t owner,
                                  int imagefd, int slotfd,
                                  char *err, size_t errlen)
{
    int lockfd = -1;
    r3c_txn_record_t rec;
    char src_hex[65], rb_hex[65];
    uint64_t src_len = 0, rb_len = 0, off = 0;
    unsigned char buf[16384];
    int rc = -1;

    if (imagefd < 0 || slotfd < 0) return fail(err, errlen, "invalid image/slot fd");
    if (r3c_txn_acquire_lock(txdir, owner, &lockfd, err, errlen) != 0) return -1;
    if (r3c_txn_load_by_id(txdir, id, owner, &rec, err, errlen) != 0) goto out;
    if (rec.state != R3C_TXN_VERIFIED || rec.consumed) {
        fail(err, errlen, "transaction not in VERIFIED state");
        goto out;
    }
    if (r3c_txn_digest_fd(imagefd, 0, src_hex, &src_len) != 0 ||
        strcmp(src_hex, rec.image_sha256) != 0) {
        fail(err, errlen, "verified image digest mismatch before slot write");
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
    if (lockfd >= 0) { (void)flock(lockfd, LOCK_UN); close(lockfd); }
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
                          int slotfd,
                          char *err, size_t errlen)
{
    r3c_txn_record_t rec;
    char slot_hex[65];
    uint64_t slot_len = 0;

    if (!valid_boot_id(boot_id) || bootcount == 0)
        return fail(err, errlen, "invalid boot_id or bootcount");
    if (r3c_txn_load_by_id(txdir, id, owner, &rec, err, errlen) != 0) return -1;
    if (rec.consumed || (rec.state != R3C_TXN_PENDING && rec.state != R3C_TXN_CONFIRMING))
        return fail(err, errlen, "transaction not pending/confirmable");
    if ((ab_slot_t)rec.target_slot != running_slot ||
        (ab_slot_t)rec.last_good_slot != last_good_slot ||
        rec.written_size == 0)
        return fail(err, errlen, "transaction slot or size mismatch");
    if (slotfd < 0 ||
        r3c_txn_digest_fd(slotfd, rec.written_size, slot_hex, &slot_len) != 0 ||
        slot_len != rec.written_size || strcmp(slot_hex, rec.image_sha256) != 0)
        return fail(err, errlen, "installed slot digest verification failed");

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
