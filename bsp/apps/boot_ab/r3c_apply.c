#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "r3c_apply.h"
#include "r3c_bundle.h"
#include "r3c_data.h"
#include "r3c_txn.h"
#include "ota_state_io.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define MAX_IMAGE_BYTES    (2ULL * 1024 * 1024 * 1024)
#define MAX_KERNEL_BYTES   (64ULL * 1024 * 1024)
#define MAX_DTB_BYTES      (16ULL * 1024 * 1024)
#define MAX_MANIFEST_BYTES (64U * 1024U)
#define MAX_MOUNTINFO_BYTES (1024U * 1024U)

static int fail(char *err, size_t len, const char *msg)
{
    if (err && len)
        snprintf(err, len, "%s", msg);
    return -1;
}

static int safe_abs(const char *s)
{
    size_t n;
    if (!s || s[0] != '/' || s[1] == '\0')
        return 0;
    n = strlen(s);
    if (n >= PATH_MAX || s[n - 1] == '/' || strstr(s, "//") ||
        strstr(s, "/../") || strstr(s, "/./") || strpbrk(s, " \t\r\n\\"))
        return 0;
    if (n >= 3 && strcmp(s + n - 3, "/..") == 0)
        return 0;
    if (n >= 2 && strcmp(s + n - 2, "/.") == 0)
        return 0;
    return 1;
}

static int valid_hex64(const char *s)
{
    size_t i;
    int nonzero = 0;
    if (!s || strlen(s) != 64)
        return 0;
    for (i = 0; i < 64; ++i) {
        if (!((s[i] >= '0' && s[i] <= '9') || (s[i] >= 'a' && s[i] <= 'f')))
            return 0;
        if (s[i] != '0')
            nonzero = 1;
    }
    return nonzero;
}

static int trusted_parent_dirs(const char *path, uid_t owner)
{
    char buf[PATH_MAX], *slash;
    struct stat st;
    size_t i, n;

    if (!safe_abs(path))
        return -1;
    snprintf(buf, sizeof(buf), "%s", path);
    slash = strrchr(buf, '/');
    if (!slash)
        return -1;
    if (lstat("/", &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022))
        return -1;
    if (slash == buf)
        return 0;
    *slash = '\0';
    n = strlen(buf);
    for (i = 1; i <= n; ++i) {
        char saved;
        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        saved = buf[i];
        buf[i] = '\0';
        if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode) ||
            (st.st_uid != 0 && st.st_uid != owner) || (st.st_mode & 0022))
            return -1;
        buf[i] = saved;
    }
    return 0;
}

static int trusted_dirs(const char *path, uid_t owner)
{
    char buf[PATH_MAX];
    struct stat st;
    size_t i, n;

    if (!safe_abs(path))
        return -1;
    n = strlen(path);
    memcpy(buf, path, n + 1);
    if (lstat("/", &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != 0 || (st.st_mode & 0022))
        return -1;
    for (i = 1; i <= n; ++i) {
        char saved;
        if (buf[i] != '/' && buf[i] != '\0')
            continue;
        saved = buf[i];
        buf[i] = '\0';
        if (lstat(buf, &st) != 0 || !S_ISDIR(st.st_mode) ||
            (st.st_uid != 0 && st.st_uid != owner) || (st.st_mode & 0022))
            return -1;
        buf[i] = saved;
        if (saved == '\0')
            break;
    }
    return 0;
}

static int open_verified_dir(const char *path, uid_t owner, int require_private,
                             int *out_fd, char *err, size_t errlen, const char *label)
{
    struct stat st;
    int fd;
    mode_t forbidden = require_private ? 0077 : 0022;

    *out_fd = -1;
    if (trusted_dirs(path, owner) != 0)
        return fail(err, errlen, label);
    fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0 || fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != owner || (st.st_mode & forbidden)) {
        if (fd >= 0)
            close(fd);
        return fail(err, errlen, label);
    }
    *out_fd = fd;
    return 0;
}

static int open_verified_reg_at(int dirfd, const char *name, uid_t owner,
                                uint64_t max_bytes, struct stat *out_st)
{
    struct stat st;
    int fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0)
        return -1;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != owner ||
        (st.st_mode & 0022) || st.st_nlink != 1 || st.st_size <= 0 ||
        (uint64_t)st.st_size > max_bytes) {
        close(fd);
        return -1;
    }
    if (out_st)
        *out_st = st;
    return fd;
}

static int read_exact_small(int fd, char *buf, size_t cap, size_t *out_len)
{
    struct stat before, after;
    size_t used = 0;

    if (fstat(fd, &before) != 0 || before.st_size <= 0 || (size_t)before.st_size > cap)
        return -1;
    while (used < (size_t)before.st_size) {
        ssize_t n = pread(fd, buf + used, (size_t)before.st_size - used, (off_t)used);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        used += (size_t)n;
    }
    if (fstat(fd, &after) != 0 || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec)
        return -1;
    buf[used] = '\0';
    *out_len = used;
    return 0;
}

static int read_mountinfo_bounded(const char *path, char **out_buf)
{
    FILE *fp;
    char *line = NULL, *buf = NULL;
    size_t cap = 0, used = 0;
    ssize_t n;

    *out_buf = NULL;
    fp = fopen((path && path[0]) ? path : "/proc/self/mountinfo", "re");
    if (!fp)
        return -1;
    while ((n = getline(&line, &cap, fp)) > 0) {
        char *nx;
        if ((size_t)n > MAX_MOUNTINFO_BYTES - used) {
            free(buf);
            buf = NULL;
            break;
        }
        nx = realloc(buf, used + (size_t)n + 1);
        if (!nx) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = nx;
        memcpy(buf + used, line, (size_t)n);
        used += (size_t)n;
        buf[used] = '\0';
    }
    if (ferror(fp)) {
        free(buf);
        buf = NULL;
    }
    free(line);
    fclose(fp);
    if (!buf || used == 0) {
        free(buf);
        return -1;
    }
    *out_buf = buf;
    return 0;
}

static int verify_data_and_open_txdir(const r3c_policy_t *policy,
                                      const r3c_apply_opts_t *opts,
                                      int *out_txdir_fd,
                                      char *err,
                                      size_t errlen)
{
    size_t mlen;
    int data_dir = -1, txdir_fd = -1;
    struct stat dst, txst, rst;
    const char *mi_path = (opts->mountinfo_path && opts->mountinfo_path[0])
                              ? opts->mountinfo_path
                              : "/proc/self/mountinfo";
    const char *fstype;

    *out_txdir_fd = -1;
    if (!safe_abs(policy->data_mountpoint) || !safe_abs(policy->tx_dir))
        return fail(err, errlen, "invalid DATA or transaction directory path");

    mlen = strlen(policy->data_mountpoint);
    if (strncmp(policy->tx_dir, policy->data_mountpoint, mlen) != 0 ||
        policy->tx_dir[mlen] != '/')
        return fail(err, errlen, "transaction directory is outside DATA mountpoint");

    if (stat("/", &rst) != 0)
        return fail(err, errlen, "cannot stat root '/'");

    if (opts->allow_fuse_data_bypass_blk) {
        char *mbuf = NULL;
        int matched;

        if (trusted_dirs(policy->data_mountpoint, policy->trusted_uid) != 0)
            return fail(err, errlen, "untrusted DATA mountpoint");
        data_dir = open(policy->data_mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (data_dir < 0 || fstat(data_dir, &dst) != 0 || !S_ISDIR(dst.st_mode) ||
            dst.st_dev == rst.st_dev) {
            if (data_dir >= 0) close(data_dir);
            return fail(err, errlen, "DATA mountpoint is not an isolated mount");
        }
        fstype = (opts->expected_data_fstype && opts->expected_data_fstype[0])
                     ? opts->expected_data_fstype
                     : "fuse.ext4";
        if (read_mountinfo_bounded(mi_path, &mbuf) != 0) {
            close(data_dir);
            return fail(err, errlen, "DATA mountinfo identity or mode invalid");
        }
        matched = r3c_data_mountinfo_matches_fstype(
            mbuf, policy->data_mountpoint, major(dst.st_dev), minor(dst.st_dev), fstype);
        free(mbuf);
        if (matched != 0) {
            close(data_dir);
            return fail(err, errlen, "DATA mountinfo identity or mode invalid");
        }
        if (r3c_data_inspect_contents(data_dir, policy->trusted_uid,
                                      policy->data_min_bytes, err, errlen) != 0 ||
            r3c_data_verify_persistence(data_dir, policy->trusted_uid, err, errlen) != 0) {
            close(data_dir);
            return -1;
        }
    } else {
        const char *excl[2] = { policy->slot1_device, policy->slot2_device };
        const r3c_sector_range_t reserved[1] = { { 1792ULL, 32ULL } };
        r3c_data_full_policy_t fp;

        memset(&fp, 0, sizeof(fp));
        fp.mountpoint = policy->data_mountpoint;
        fp.device = policy->data_device;
        fp.whole_disk_device = policy->whole_disk_device;
        fp.mountinfo_path = mi_path;
        fp.sysfs_block_root = (opts->sysfs_block_root && opts->sysfs_block_root[0])
                                  ? opts->sysfs_block_root
                                  : "/sys/dev/block";
        fp.expected_fstype = (opts->expected_data_fstype && opts->expected_data_fstype[0])
                                 ? opts->expected_data_fstype
                                 : "ext4";
        fp.excluded_devices = excl;
        fp.excluded_count = 2;
        fp.reserved_sectors = reserved;
        fp.reserved_count = 1;
        fp.minimum_available_bytes = policy->data_min_bytes;
        fp.owner = policy->trusted_uid;
        fp.verify_write_persistence = 1;

        if (r3c_data_inspect_full(&fp, err, errlen) != 0)
            return -1;
        data_dir = open(policy->data_mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (data_dir < 0 || fstat(data_dir, &dst) != 0 || !S_ISDIR(dst.st_mode)) {
            if (data_dir >= 0) close(data_dir);
            return fail(err, errlen, "DATA mountpoint inaccessible");
        }
    }

    if (open_verified_dir(policy->tx_dir, policy->trusted_uid, 1, &txdir_fd,
                          err, errlen, "untrusted transaction directory") != 0) {
        close(data_dir);
        return -1;
    }
    if (fstat(txdir_fd, &txst) != 0 || txst.st_dev != dst.st_dev || txst.st_dev == rst.st_dev) {
        close(txdir_fd);
        close(data_dir);
        return fail(err, errlen, "transaction directory is not on verified DATA mount");
    }
    close(data_dir);
    *out_txdir_fd = txdir_fd;
    return 0;
}

static int lookup_manifest_digest(const char *manifest_buf, const char *target_path,
                                  char out_hex[65])
{
    const char *p = manifest_buf;
    size_t target_len = strlen(target_path);

    while (*p) {
        const char *nl = strchr(p, '\n');
        const char *sp = strchr(p, ' ');
        size_t line_len = nl ? (size_t)(nl - p) : strlen(p);

        if (sp && (size_t)(sp - p) < line_len &&
            (size_t)(sp - p) == target_len &&
            memcmp(p, target_path, target_len) == 0) {
            size_t hash_len = line_len - (size_t)(sp + 1 - p);
            if (hash_len == 64) {
                memcpy(out_hex, sp + 1, 64);
                out_hex[64] = '\0';
                return valid_hex64(out_hex) ? 0 : -1;
            }
            return -1;
        }
        if (!nl)
            break;
        p = nl + 1;
    }
    return -1;
}

static int verify_app_ota_settled_locked(const char *app_ota_dir, uid_t owner,
                                         int *out_lockfd, char *err, size_t errlen)
{
    int app_dir = -1, lockfd = -1, state_dir = -1, state_fd = -1;
    struct stat st;
    char state_path[PATH_MAX];
    ota_state_record_t app_state;

    *out_lockfd = -1;
    if (open_verified_dir(app_ota_dir, owner, 0, &app_dir, err, errlen,
                          "untrusted application OTA directory") != 0)
        goto fail_out;

    lockfd = openat(app_dir, ".ota.lock", O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (lockfd < 0 || fstat(lockfd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != owner || (st.st_mode & 0022) || st.st_nlink != 1) {
        fail(err, errlen, "app OTA lock (.ota.lock) missing or untrusted");
        goto fail_out;
    }
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0) {
        fail(err, errlen, "app OTA lock (.ota.lock) is busy");
        goto fail_out;
    }

    state_dir = openat(app_dir, "state", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (state_dir < 0 || fstat(state_dir, &st) != 0 || !S_ISDIR(st.st_mode) ||
        st.st_uid != owner || (st.st_mode & 0022)) {
        fail(err, errlen, "app OTA state directory missing or untrusted");
        goto fail_out;
    }
    state_fd = openat(state_dir, "ota_state.json", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (state_fd < 0 || fstat(state_fd, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_uid != owner || (st.st_mode & 0022) || st.st_nlink != 1) {
        fail(err, errlen, "app OTA state file missing or untrusted");
        goto fail_out;
    }
    close(state_fd);
    state_fd = -1;

    if (snprintf(state_path, sizeof(state_path), "%s/state", app_ota_dir) >= (int)sizeof(state_path) ||
        ota_state_load(state_path, &app_state) != 0 ||
        (app_state.state != OTA_CONFIRMED && app_state.state != OTA_ROLLED_BACK)) {
        fail(err, errlen, "application OTA is not in a settled terminal state");
        goto fail_out;
    }

    close(state_dir);
    close(app_dir);
    *out_lockfd = lockfd;
    return 0;

fail_out:
    if (state_fd >= 0) close(state_fd);
    if (state_dir >= 0) close(state_dir);
    if (lockfd >= 0) { (void)flock(lockfd, LOCK_UN); close(lockfd); }
    if (app_dir >= 0) close(app_dir);
    return -1;
}

static int precheck_target_capacity(const char *target_path, uid_t owner, int allow_reg,
                                    uint64_t required_bytes, char *err, size_t errlen)
{
    struct stat lst, fst;
    uint64_t cap = 0;
    int fd = -1;

    if (!target_path || !safe_abs(target_path) || trusted_parent_dirs(target_path, owner) != 0)
        return fail(err, errlen, "untrusted target slot path");
    if (lstat(target_path, &lst) != 0)
        return fail(err, errlen, "target slot device missing or inaccessible");
    if (S_ISLNK(lst.st_mode))
        return fail(err, errlen, "target slot cannot be symlink");

    fd = open(target_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0 || fstat(fd, &fst) != 0) {
        if (fd >= 0) close(fd);
        return fail(err, errlen, "cannot inspect target slot capacity");
    }
    if ((fst.st_mode & S_IFMT) != (lst.st_mode & S_IFMT) ||
        fst.st_dev != lst.st_dev || fst.st_ino != lst.st_ino) {
        close(fd);
        return fail(err, errlen, "target slot changed between lstat and open");
    }
    if (S_ISBLK(fst.st_mode)) {
        if (ioctl(fd, BLKGETSIZE64, &cap) != 0) {
            close(fd);
            return fail(err, errlen, "cannot query target block device size");
        }
    } else if (allow_reg && S_ISREG(fst.st_mode)) {
        cap = (uint64_t)fst.st_size;
    } else {
        close(fd);
        return fail(err, errlen, "target slot is not a block device");
    }
    close(fd);

    if (cap < required_bytes)
        return fail(err, errlen, "target slot capacity smaller than rootfs.img");
    return 0;
}

static void cleanup_failed_verified_txn(int txdir_fd, const char *txid, uid_t owner)
{
    int lockfd = -1;
    r3c_txn_record_t rec;
    char err_ignore[128];
    char txn_name[80], man_name[96];

    if (txdir_fd < 0 || !txid || !txid[0])
        return;
    if (r3c_txn_acquire_lock(txdir_fd, owner, &lockfd, err_ignore, sizeof(err_ignore)) != 0)
        return;
    if (r3c_txn_load_by_id(txdir_fd, txid, owner, &rec, err_ignore, sizeof(err_ignore)) == 0 &&
        rec.state == R3C_TXN_VERIFIED && !rec.consumed) {
        if (snprintf(txn_name, sizeof(txn_name), "txn-%s", txid) < (int)sizeof(txn_name))
            (void)unlinkat(txdir_fd, txn_name, 0);
        if (snprintf(man_name, sizeof(man_name), "txn-%s.manifest", txid) < (int)sizeof(man_name))
            (void)unlinkat(txdir_fd, man_name, 0);
        (void)fsync(txdir_fd);
    }
    if (lockfd >= 0) {
        (void)flock(lockfd, LOCK_UN);
        close(lockfd);
    }
}

int r3c_coordinator_apply_bundle(const r3c_policy_t *policy,
                                 const fw_env_backend_cfg_t *fw_cfg,
                                 const ab_health_cfg_t *hcfg,
                                 const r3c_apply_opts_t *opts,
                                 char out_txid[65],
                                 ab_slot_t *out_last_good,
                                 char *errbuf,
                                 size_t errlen)
{
    int rc = -1;
    int app_lockfd = -1, bundle_fd = -1, txdir_fd = -1, key_fd = -1;
    int img_fd = -1, zimg_fd = -1, dtb_fd = -1, rm_fd = -1;
    char *rm_buf = NULL;
    size_t rm_len = 0;
    struct stat img_st, zimg_st, dtb_st, rm_st, key_st;
    ab_slot_t running_slot = AB_SLOT_NONE;
    ab_env_t env;
    const char *mi_path = NULL;
    const char *target_path = NULL;
    const char *policy_slot_dev = NULL;
    char z_hex[65], dtb_hex[65], mf_z_hex[65], mf_dtb_hex[65];
    uint64_t z_len = 0, dtb_len = 0;
    char txid[65] = {0};
    r3c_slot_write_opts_t wopts;
    const r3c_slot_write_opts_t *wopts_ptr = NULL;

    if (!policy || !fw_cfg || !hcfg || !opts || !opts->bundle_dir)
        return fail(errbuf, errlen, "invalid arguments to apply-bundle");
    if (opts->target_slot != 1 && opts->target_slot != 2)
        return fail(errbuf, errlen, "invalid target slot");
    if (opts->has_expected_target && opts->expected_target != opts->target_slot)
        return fail(errbuf, errlen, "target slot disagrees with expected_target");

    /* Gate 1: explicit factory p2 write protection before touching txdir, slot, or env */
    if (policy->protect_slot2 && opts->target_slot == 2)
        return fail(errbuf, errlen, "target slot 2 is write-protected factory baseline");

    if (out_txid)
        out_txid[0] = '\0';
    if (out_last_good)
        *out_last_good = AB_SLOT_NONE;

    if (open_verified_dir(opts->bundle_dir, policy->trusted_uid, 1, &bundle_fd,
                          errbuf, errlen, "untrusted or invalid bundle directory") != 0)
        goto out;

    if (verify_app_ota_settled_locked(policy->app_ota_dir, policy->trusted_uid,
                                      &app_lockfd, errbuf, errlen) != 0)
        goto out;

    mi_path = (opts->mountinfo_path && opts->mountinfo_path[0]) ? opts->mountinfo_path : "/proc/self/mountinfo";
    if (ab_detect_running_slot_full(hcfg, mi_path, opts->allow_host_slot_sim,
                                    &running_slot, errbuf, errlen) != 0)
        goto out;
    if (fw_env_backend_load(fw_cfg, &env, errbuf, errlen) != 0)
        goto out;
    if (ab_env_validate(&env, errbuf, errlen) != 0)
        goto out;
    if (env.ab_state == AB_STATE_TRIAL || env.upgrade_available != 0) {
        fail(errbuf, errlen, "refusing system OTA while A/B trial upgrade is active");
        goto out;
    }
    if ((ab_slot_t)opts->target_slot == running_slot ||
        (ab_slot_t)opts->target_slot == env.last_good_slot ||
        (ab_slot_t)opts->target_slot == env.active_slot ||
        (ab_slot_t)opts->target_slot != ab_slot_other(running_slot) ||
        env.last_good_slot != running_slot) {
        fail(errbuf, errlen, "target slot conflicts with running or last_good slot");
        goto out;
    }

    /* Gate 2: verify shared DATA (p3) identity, schema=1, persistence, and tx_dir containment */
    if (verify_data_and_open_txdir(policy, opts, &txdir_fd, errbuf, errlen) != 0)
        goto out;

    if (env.ab_state == AB_STATE_ROLLED_BACK && env.upgrade_available == 0 &&
        env.pending_slot == AB_SLOT_NONE) {
        char cur_boot_id[64] = {0};
        int bid_fd = open("/proc/sys/kernel/random/boot_id", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        int reconciled = 0;
        if (bid_fd >= 0) {
            ssize_t bn = read(bid_fd, cur_boot_id, sizeof(cur_boot_id) - 1);
            close(bid_fd);
            if (bn > 0) {
                cur_boot_id[bn] = '\0';
                for (ssize_t k = 0; k < bn; ++k) {
                    if (cur_boot_id[k] == '\n' || cur_boot_id[k] == '\r') {
                        cur_boot_id[k] = '\0';
                        break;
                    }
                }
            } else {
                cur_boot_id[0] = '\0';
            }
        }
        if (r3c_txn_reconcile_rollback(txdir_fd, policy->trusted_uid, &env, running_slot,
                                       cur_boot_id[0] ? cur_boot_id : NULL,
                                       &reconciled, NULL, errbuf, errlen) != 0)
            goto out;
    }

    if (trusted_parent_dirs(policy->pubkey_path, policy->trusted_uid) != 0) {
        fail(errbuf, errlen, "untrusted public key path");
        goto out;
    }
    key_fd = open(policy->pubkey_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (key_fd < 0 || fstat(key_fd, &key_st) != 0 || !S_ISREG(key_st.st_mode) ||
        key_st.st_uid != policy->trusted_uid || (key_st.st_mode & 0022) ||
        key_st.st_nlink != 1 || key_st.st_size <= 0 || key_st.st_size > 16384) {
        fail(errbuf, errlen, "OTA public key missing or untrusted");
        goto out;
    }

    img_fd = open_verified_reg_at(bundle_fd, "rootfs.img", policy->trusted_uid, MAX_IMAGE_BYTES, &img_st);
    zimg_fd = open_verified_reg_at(bundle_fd, "zImage", policy->trusted_uid, MAX_KERNEL_BYTES, &zimg_st);
    dtb_fd = open_verified_reg_at(bundle_fd, "100ask_imx6ull-14x14.dtb", policy->trusted_uid, MAX_DTB_BYTES, &dtb_st);
    rm_fd = open_verified_reg_at(bundle_fd, "rootfs.manifest", policy->trusted_uid, MAX_MANIFEST_BYTES, &rm_st);
    if (img_fd < 0 || zimg_fd < 0 || dtb_fd < 0 || rm_fd < 0) {
        fail(errbuf, errlen, "bundle missing required signed payload files or invalid permissions");
        goto out;
    }

    rm_buf = malloc((size_t)rm_st.st_size + 1);
    if (!rm_buf || read_exact_small(rm_fd, rm_buf, (size_t)rm_st.st_size, &rm_len) != 0 ||
        rm_len == 0 || rm_buf[rm_len - 1] != '\n') {
        fail(errbuf, errlen, "invalid bundle rootfs.manifest");
        goto out;
    }
    if (r3c_txn_digest_fd(zimg_fd, 0, z_hex, &z_len) != 0 ||
        r3c_txn_digest_fd(dtb_fd, 0, dtb_hex, &dtb_len) != 0 ||
        lookup_manifest_digest(rm_buf, "/boot/zImage", mf_z_hex) != 0 ||
        lookup_manifest_digest(rm_buf, "/boot/100ask_imx6ull-14x14.dtb", mf_dtb_hex) != 0 ||
        strcmp(z_hex, mf_z_hex) != 0 || strcmp(dtb_hex, mf_dtb_hex) != 0) {
        fail(errbuf, errlen, "bundle kernel/dtb digest diverges from rootfs.manifest");
        goto out;
    }

    policy_slot_dev = (opts->target_slot == 1) ? policy->slot1_device : policy->slot2_device;
    if (!opts->target_dev_override || !opts->target_dev_override[0]) {
        const char *ab_dev = ab_slot_device((ab_slot_t)opts->target_slot);
        if (!ab_dev || strcmp(policy_slot_dev, ab_dev) != 0) {
            fail(errbuf, errlen, "policy slot device disagrees with A/B layout");
            goto out;
        }
        target_path = ab_dev;
    } else {
        target_path = opts->target_dev_override;
    }

    if (precheck_target_capacity(target_path, policy->trusted_uid,
                                 opts->allow_regular_file_target,
                                 (uint64_t)img_st.st_size, errbuf, errlen) != 0)
        goto out;

    {
        r3c_bundle_compat_t compat;
        uint32_t cur_schema = 0;
        int data_dir_fd = open(policy->data_mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (data_dir_fd < 0 ||
            r3c_data_read_schema(data_dir_fd, policy->trusted_uid, &cur_schema, errbuf, errlen) != 0) {
            if (data_dir_fd >= 0) close(data_dir_fd);
            if (!errbuf[0]) fail(errbuf, errlen, "failed reading DATA schema");
            goto out;
        }
        close(data_dir_fd);
        compat.boot_cap = policy->boot_cap ? policy->boot_cap : 1u;
        compat.min_sys_version = policy->min_sys_version;
        compat.data_schema = cur_schema;
        compat.require_compat_manifest = policy->require_compat_manifest;
        if (errbuf && errlen) errbuf[0] = '\0';
        if (r3c_bundle_record_checked(bundle_fd, txdir_fd, key_fd, policy->trusted_uid,
                                      opts->target_slot, (unsigned int)running_slot,
                                      &compat, txid, errbuf, errlen) != 0) {
            if (!errbuf[0]) fail(errbuf, errlen, "signed bundle verification failed");
            goto out;
        }
    }

    if (opts->allow_regular_file_target || opts->allow_host_slot_sim ||
        (opts->target_dev_override && opts->target_dev_override[0]) ||
        (opts->mountinfo_path && opts->mountinfo_path[0])) {
        memset(&wopts, 0, sizeof(wopts));
        wopts.target_dev_override = opts->target_dev_override;
        wopts.mountinfo_path = opts->mountinfo_path;
        wopts.sysfs_block_root = opts->sysfs_block_root;
        wopts.allow_regular_file_target = opts->allow_regular_file_target ? 1 : 0;
        wopts.allow_host_slot_sim = opts->allow_host_slot_sim ? 1 : 0;
        wopts_ptr = &wopts;
    }

    if (r3c_txn_write_and_verify_slot(txdir_fd, txid, policy->trusted_uid, img_fd,
                                      fw_cfg, hcfg, wopts_ptr, errbuf, errlen) != 0) {
        cleanup_failed_verified_txn(txdir_fd, txid, policy->trusted_uid);
        goto out;
    }

    if (r3c_txn_stage_pending(txdir_fd, txid, policy->trusted_uid,
                              fw_cfg, hcfg, running_slot, errbuf, errlen) != 0)
        goto out;

    if (out_txid)
        memcpy(out_txid, txid, 65);
    if (out_last_good)
        *out_last_good = running_slot;
    rc = 0;

out:
    free(rm_buf);
    if (rm_fd >= 0) close(rm_fd);
    if (dtb_fd >= 0) close(dtb_fd);
    if (zimg_fd >= 0) close(zimg_fd);
    if (img_fd >= 0) close(img_fd);
    if (key_fd >= 0) close(key_fd);
    if (txdir_fd >= 0) close(txdir_fd);
    if (app_lockfd >= 0) { (void)flock(app_lockfd, LOCK_UN); close(app_lockfd); }
    if (bundle_fd >= 0) close(bundle_fd);
    return rc;
}
