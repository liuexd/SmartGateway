#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "r3c_bundle.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_IMAGE (2ULL * 1024 * 1024 * 1024)
#define MAX_ROOTFS_MANIFEST (64 * 1024)

static int secure_file(int fd, uid_t owner, off_t max)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == owner &&
           !(st.st_mode & 0022) && st.st_nlink == 1 && st.st_size > 0 &&
           st.st_size <= max ? 0 : -1;
}

static int secure_dir(int fd, uid_t owner)
{
    struct stat st;
    return fstat(fd, &st) == 0 && S_ISDIR(st.st_mode) && st.st_uid == owner &&
           !(st.st_mode & 0077) ? 0 : -1;
}

static int read_small(int fd, unsigned char *buf, size_t cap, size_t *len)
{
    struct stat st, after;
    size_t used = 0;
    if (fstat(fd, &st) != 0 || st.st_size <= 0 || (size_t)st.st_size > cap) return -1;
    while (used < (size_t)st.st_size) {
        ssize_t n = pread(fd, buf + used, (size_t)st.st_size - used, (off_t)used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    if (fstat(fd, &after) != 0 || st.st_size != after.st_size ||
        st.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        st.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        st.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        st.st_ctim.tv_nsec != after.st_ctim.tv_nsec) return -1;
    *len = used;
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

#include <sys/wait.h>

static const char *const MANDATORY_ROOTFS_PATHS[] = {
    "/sbin/init",
    "/etc/inittab",
    "/etc/fstab",
    "/etc/fw_env.config",
    "/sbin/fw_printenv",
    "/usr/bin/sgw_bootctl",
    "/usr/bin/sgw_watchdog_guard",
    "/usr/bin/service_supervisor",
    "/usr/bin/smart_gateway",
    "/usr/bin/ota_agent",
    "/etc/init.d/S15sgw_ab_guard",
    "/etc/init.d/S99smartgateway",
    "/boot/zImage",
    "/boot/100ask_imx6ull-14x14.dtb",
    "/lib/libc.so.6"
};
#define MANDATORY_ROOTFS_COUNT (sizeof(MANDATORY_ROOTFS_PATHS) / sizeof(MANDATORY_ROOTFS_PATHS[0]))

static int valid_manifest_relpath(const char *p)
{
    size_t i, n;
    if (!p || p[0] != '/' || p[1] == '\0') return 0;
    n = strlen(p);
    if (n >= 256 || p[n - 1] == '/' || strstr(p, "//") ||
        strstr(p, "/../") || strstr(p, "/./"))
        return 0;
    if (n >= 3 && strcmp(p + n - 3, "/..") == 0) return 0;
    if (n >= 2 && strcmp(p + n - 2, "/.") == 0) return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)p[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-'))
            return 0;
    }
    return 1;
}

/* Restrict the only supported symlink to a same-directory glibc target. */
static int valid_libc_target(const char *name)
{
    size_t i, n = strlen(name);
    if (n < 9 || n >= 64 || strncmp(name, "libc-", 5) ||
        strcmp(name + n - 3, ".so")) return 0;
    for (i = 5; i < n - 3; ++i)
        if ((name[i] < '0' || name[i] > '9') && name[i] != '.') return 0;
    return 1;
}

static int run_debugfs_cmd(const char *debugfs_bin, const char *cmd_arg, int dup_fd,
                           int capture_sha256, char out_hex[65], int *out_is_regular,
                           long long total_deadline)
{
    int pipes[2] = {-1, -1}, status = 0;
    pid_t pid;
    char fdpath[64];
    unsigned char buf[8192], digest[EVP_MAX_MD_SIZE];
    char text[4096];
    size_t text_used = 0;
    unsigned int dlen = 0;
    EVP_MD_CTX *ctx = NULL;
    ssize_t n;
    static const char tbl[] = "0123456789abcdef";
    struct timespec clock_start, clock_now;
    long long deadline;
    int child_done = 0, timed_out = 0, read_failed = 0;

    snprintf(fdpath, sizeof(fdpath), "/proc/self/fd/%d", dup_fd);
    if (pipe(pipes) != 0) return -1;
    pid = fork();
    if (pid < 0) { close(pipes[0]); close(pipes[1]); return -1; }
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        close(pipes[0]);
        if (pipes[1] != STDOUT_FILENO) { dup2(pipes[1], STDOUT_FILENO); close(pipes[1]); }
        if (devnull >= 0) { dup2(devnull, STDERR_FILENO); if (devnull > 2) close(devnull); }
        {
            char *argv_df[] = { (char *)debugfs_bin, "-R", (char *)cmd_arg, fdpath, NULL };
            execv(debugfs_bin, argv_df);
        }
        _exit(127);
    }
    close(pipes[1]);
    if (clock_gettime(CLOCK_MONOTONIC, &clock_start) != 0) {
        close(pipes[0]); (void)kill(pid, SIGKILL);
        (void)waitpid(pid, &status, WNOHANG); return -1;
    }
    deadline = (long long)clock_start.tv_sec * 1000 + clock_start.tv_nsec / 1000000 + 10000;
    if (total_deadline < deadline) deadline = total_deadline;
    if (capture_sha256) {
        ctx = EVP_MD_CTX_new();
        if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) {
            close(pipes[0]);
            (void)kill(pid, SIGKILL);
            (void)waitpid(pid, &status, WNOHANG);
            if (ctx) EVP_MD_CTX_free(ctx);
            return -1;
        }
    }
    for (;;) {
        struct pollfd pfd = {pipes[0], POLLIN | POLLHUP, 0};
        long long now;
        int ready;
        if (clock_gettime(CLOCK_MONOTONIC, &clock_now) != 0) { timed_out = 1; break; }
        now = (long long)clock_now.tv_sec * 1000 + clock_now.tv_nsec / 1000000;
        if (now >= deadline) { timed_out = 1; break; }
        ready = poll(&pfd, 1, (int)(deadline - now));
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || (pfd.revents & (POLLERR | POLLNVAL))) { timed_out = 1; break; }
        n = read(pipes[0], buf, sizeof(buf));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { read_failed = 1; break; }
        if (n == 0) break;
        if (capture_sha256) {
            if (EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) { read_failed = 1; break; }
        } else {
            if ((size_t)n > sizeof(text) - 1 - text_used) { read_failed = 1; break; }
            memcpy(text + text_used, buf, (size_t)n);
            text_used += (size_t)n;
        }
    }
    close(pipes[0]);
    while (!timed_out && !read_failed) {
        pid_t waited = waitpid(pid, &status, WNOHANG);
        if (waited == pid) { child_done = 1; break; }
        if (waited < 0 && errno != EINTR) break;
        if (clock_gettime(CLOCK_MONOTONIC, &clock_now) != 0 ||
            (long long)clock_now.tv_sec * 1000 + clock_now.tv_nsec / 1000000 >= deadline) {
            timed_out = 1; break;
        }
        { struct timespec delay = {0, 5000000L}; nanosleep(&delay, NULL); }
    }
    if (!child_done) { (void)kill(pid, SIGKILL); (void)waitpid(pid, &status, WNOHANG); }
    if (!child_done || timed_out || read_failed || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        EVP_MD_CTX_free(ctx); return -1;
    }
    if (capture_sha256) {
        if (EVP_DigestFinal_ex(ctx, digest, &dlen) != 1 || dlen != 32) {
            EVP_MD_CTX_free(ctx); return -1;
        }
        EVP_MD_CTX_free(ctx);
        for (size_t i = 0; i < 32; ++i) {
            out_hex[i * 2] = tbl[digest[i] >> 4];
            out_hex[i * 2 + 1] = tbl[digest[i] & 15];
        }
        out_hex[64] = '\0';
    } else {
        text[text_used] = '\0';
        if (out_is_regular) {
            *out_is_regular = (strstr(text, "Type: regular") != NULL) ? 1 : 0;
            if (strstr(text, "Type: symlink") != NULL) {
                const char *begin = strstr(text, "Fast link dest: \"");
                const char *end;
                if (begin) {
                    begin += strlen("Fast link dest: \"");
                    end = strchr(begin, '"');
                    if (end && end > begin && (size_t)(end - begin) < 64) {
                        memcpy(out_hex, begin, (size_t)(end - begin));
                        out_hex[end - begin] = '\0';
                        if (valid_libc_target(out_hex)) *out_is_regular = 2;
                    }
                }
            }
        }
    }
    return 0;
}

static const char *find_debugfs_bin(void)
{
    if (access("/usr/sbin/debugfs", X_OK) == 0) return "/usr/sbin/debugfs";
    if (access("/sbin/debugfs", X_OK) == 0) return "/sbin/debugfs";
    return NULL;
}

static int verify_manifest_matches_ext4_image(int imagefd, const unsigned char *buf, size_t len)
{
    unsigned char sb[2];
    const char *df = find_debugfs_bin();
    int dup_fd, rc = -1;
    char *copy = NULL, *line, *save = NULL;
    struct timespec started;
    long long total_deadline;

    if (clock_gettime(CLOCK_MONOTONIC, &started) != 0) return -1;
    total_deadline = (long long)started.tv_sec * 1000 + started.tv_nsec / 1000000 + 60000;
    /* Check ext4 superblock magic 0xEF53 at offset 0x438 (little-endian: 0x53, 0xEF) */
    if (!df || pread(imagefd, sb, 2, 0x438) != 2 || sb[0] != 0x53 || sb[1] != 0xEF)
        return -1;
    dup_fd = dup(imagefd); /* Without FD_CLOEXEC so debugfs child can read /proc/self/fd/<fd> */
    if (dup_fd < 0) return -1;
    copy = malloc(len + 1);
    if (!copy) { close(dup_fd); return -1; }
    memcpy(copy, buf, len);
    copy[len] = '\0';

    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *sp = strchr(line, ' ');
        char stat_cmd[320], dump_cmd[340], actual_hex[65];
        int is_reg = 0;
        if (!sp) goto out;
        *sp = '\0';
        snprintf(stat_cmd, sizeof(stat_cmd), "stat %s", line);
        snprintf(dump_cmd, sizeof(dump_cmd), "dump %s /dev/stdout", line);
        if (run_debugfs_cmd(df, stat_cmd, dup_fd, 0, actual_hex, &is_reg, total_deadline) != 0 || !is_reg)
            goto out;
        if (is_reg == 2) {
            char target[96];
            if (strcmp(line, "/lib/libc.so.6") != 0 ||
                snprintf(target, sizeof(target), "/lib/%s", actual_hex) >= (int)sizeof(target))
                goto out;
            snprintf(stat_cmd, sizeof(stat_cmd), "stat %s", target);
            snprintf(dump_cmd, sizeof(dump_cmd), "dump %s /dev/stdout", target);
            is_reg = 0;
            if (run_debugfs_cmd(df, stat_cmd, dup_fd, 0, actual_hex, &is_reg, total_deadline) != 0 ||
                is_reg != 1) goto out;
        }
        if (run_debugfs_cmd(df, dump_cmd, dup_fd, 1, actual_hex, NULL, total_deadline) != 0 ||
            strcmp(actual_hex, sp + 1) != 0)
            goto out;
    }
    rc = 0;
out:
    free(copy);
    close(dup_fd);
    return rc;
}

static int validate_rootfs_manifest_bytes(const unsigned char *buf, size_t len)
{
    char *copy, *line, *save = NULL;
    char seen_paths[256][256];
    int count = 0, i;
    unsigned int mandatory_mask = 0;
    if (!buf || len == 0 || len > MAX_ROOTFS_MANIFEST || memchr(buf, '\0', len)) return -1;
    if (buf[len - 1] != '\n') return -1;
    copy = malloc(len + 1);
    if (!copy) return -1;
    memcpy(copy, buf, len);
    copy[len] = '\0';
    for (line = strtok_r(copy, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char *sp = strchr(line, ' ');
        if (!sp || strchr(sp + 1, ' ') != NULL) { free(copy); return -1; }
        *sp = '\0';
        if (!valid_manifest_relpath(line) || !valid_hex64(sp + 1) || count >= 256) {
            free(copy);
            return -1;
        }
        for (i = 0; i < count; ++i) {
            if (strcmp(seen_paths[i], line) == 0) { free(copy); return -1; }
        }
        snprintf(seen_paths[count], sizeof(seen_paths[count]), "%s", line);
        for (size_t m = 0; m < MANDATORY_ROOTFS_COUNT; ++m) {
            if (strcmp(line, MANDATORY_ROOTFS_PATHS[m]) == 0)
                mandatory_mask |= (1u << m);
        }
        count++;
    }
    free(copy);
    return (count >= (int)MANDATORY_ROOTFS_COUNT &&
            mandatory_mask == ((1u << MANDATORY_ROOTFS_COUNT) - 1u)) ? 0 : -1;
}

static int digest_fd(int fd, char hex[65], unsigned long long *length)
{
    static const char table[] = "0123456789abcdef";
    unsigned char buf[16384], digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    struct stat before, after;
    EVP_MD_CTX *ctx = NULL;
    off_t offset = 0;
    int rc = -1;
    size_t i;
    if (fstat(fd, &before) != 0 || before.st_size <= 0 ||
        (unsigned long long)before.st_size > MAX_IMAGE) return -1;
    ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1) goto out;
    while (offset < before.st_size) {
        size_t want = (size_t)(before.st_size - offset);
        ssize_t n;
        if (want > sizeof(buf)) want = sizeof(buf);
        n = pread(fd, buf, want, offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0 || EVP_DigestUpdate(ctx, buf, (size_t)n) != 1) goto out;
        offset += n;
    }
    if (fstat(fd, &after) != 0 || before.st_size != after.st_size ||
        before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len != 32) goto out;
    for (i = 0; i < 32; ++i) {
        hex[i * 2] = table[digest[i] >> 4];
        hex[i * 2 + 1] = table[digest[i] & 15];
    }
    hex[64] = '\0';
    *length = (unsigned long long)before.st_size;
    rc = 0;
out:
    EVP_MD_CTX_free(ctx);
    return rc;
}

static int random_id(char out[65])
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random[32];
    size_t used = 0, i;
    int nonzero = 0;
    while (used < sizeof(random)) {
        ssize_t n = getrandom(random + used, sizeof(random) - used, GRND_NONBLOCK);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        used += (size_t)n;
    }
    for (i = 0; i < sizeof(random); ++i) {
        nonzero |= random[i] != 0;
        out[2*i] = hex[random[i] >> 4];
        out[2*i+1] = hex[random[i] & 15];
    }
    out[64] = '\0';
    return nonzero ? 0 : -1;
}

static int write_all(int fd, const char *s, size_t n)
{
    size_t used = 0;
    while (used < n) {
        ssize_t count = write(fd, s + used, n - used);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return -1;
        used += (size_t)count;
    }
    return 0;
}

static int is_settled_txn_file(int txdir, const char *name, const char *expected_id, uid_t owner)
{
    unsigned char buf[4096];
    char id_needle[80];
    size_t len = 0;
    int fd = openat(txdir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    int n;

    if (fd < 0) return -1;
    if (secure_file(fd, owner, (off_t)(sizeof(buf) - 1)) != 0 ||
        read_small(fd, buf, sizeof(buf) - 1, &len) != 0) {
        close(fd);
        return -1;
    }
    close(fd);
    if (len < 10 || memchr(buf, '\0', len) != NULL) return -1;
    buf[len] = '\0';
    n = snprintf(id_needle, sizeof(id_needle), "\nid=%s\n", expected_id);
    if (n <= 0 || (size_t)n >= sizeof(id_needle)) return -1;
    if (strncmp((const char *)buf, "SGW-TXN-1\n", 10) != 0 ||
        (strstr((const char *)buf, "\nstate=CONFIRMED\n") == NULL &&
         strstr((const char *)buf, "\nstate=ROLLED_BACK\n") == NULL) ||
        strstr((const char *)buf, "\nconsumed=1\n") == NULL ||
        strstr((const char *)buf, id_needle) == NULL)
        return -1;
    return 0;
}

static int no_active_record(int txdir, uid_t owner)
{
    int fd = openat(txdir, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    DIR *dir;
    struct dirent *entry;
    int rc = -1;
    if (fd < 0) return -1;
    dir = fdopendir(fd);
    if (!dir) { close(fd); return -1; }
    errno = 0;
    while ((entry = readdir(dir)) != NULL) {
        const char *dname = entry->d_name;
        size_t dlen = strlen(dname);
        if (strcmp(dname, ".") == 0 || strcmp(dname, "..") == 0 ||
            strcmp(dname, ".sgw-transaction.lock") == 0) {
            errno = 0;
            continue;
        }
        if (dlen == 68 && strncmp(dname, "txn-", 4) == 0 && valid_hex64(dname + 4)) {
            if (is_settled_txn_file(txdir, dname, dname + 4, owner) != 0) goto out;
        } else if (dlen == 77 && strncmp(dname, "txn-", 4) == 0 &&
                   strcmp(dname + 68, ".manifest") == 0) {
            char hex[65], txn_name[72];
            int mfd;
            memcpy(hex, dname + 4, 64);
            hex[64] = '\0';
            if (!valid_hex64(hex)) goto out;
            mfd = openat(txdir, dname, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
            if (mfd < 0) goto out;
            if (secure_file(mfd, owner, MAX_ROOTFS_MANIFEST) != 0) { close(mfd); goto out; }
            close(mfd);
            snprintf(txn_name, sizeof(txn_name), "txn-%s", hex);
            if (is_settled_txn_file(txdir, txn_name, hex, owner) != 0) goto out;
        } else {
            goto out;
        }
        errno = 0;
    }
    if (errno == 0) rc = 0;
out:
    closedir(dir);
    return rc;
}

static int valid_version_str(const char *s)
{
    size_t i, n;
    if (!s || !s[0]) return 0;
    n = strlen(s);
    if (n > 64) return 0;
    for (i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)s[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '+' || c == '-'))
            return 0;
    }
    return 1;
}

static int parse_u32_line(const char *line, const char *prefix, uint32_t *out)
{
    size_t plen = strlen(prefix);
    unsigned long long val = 0;
    char *end = NULL;
    if (strncmp(line, prefix, plen) != 0 || line[plen] < '0' || line[plen] > '9')
        return -1;
    errno = 0;
    val = strtoull(line + plen, &end, 10);
    if (errno || !end || *end != '\0' || val > UINT32_MAX) return -1;
    *out = (uint32_t)val;
    return 0;
}

static int bfail(char *err, size_t errlen, const char *msg)
{
    if (err && errlen) snprintf(err, errlen, "%s", msg);
    return -1;
}

int r3c_bundle_record_checked(int bundle, int txdir, int keyfd, uid_t owner,
                              unsigned int target, unsigned int last_good,
                              const r3c_bundle_compat_t *compat,
                              char id[65], char *err, size_t errlen)
{
    unsigned char manifest[1024], signature[1024], digest[EVP_MAX_MD_SIZE];
    unsigned char *rm_buf = NULL;
    char image_hex[65], package_hex[65], rm_hex[65] = {0}, canonical[1024], record[1024];
    char name[72], rm_name[88], generated[65];
    static const char table[] = "0123456789abcdef";
    unsigned long long image_size, rm_size = 0;
    unsigned int digest_len = 0;
    size_t manifest_len = 0, signature_len = 0, rm_len = 0, i;
    int manifestfd = -1, sigfd = -1, imagefd = -1, rmfd = -1, rm_outfd = -1;
    int recordfd = -1, lockfd = -1, keycopy = -1, rc = -1;
    int created = 0, created_rm = 0, have_rm = 0, n;
    FILE *keyfile = NULL;
    EVP_PKEY *key = NULL;
    EVP_MD_CTX *ctx = NULL;
    if (!id) return -1;
    id[0] = '\0';
    rm_name[0] = '\0';
    if ((target != 1 && target != 2) || (last_good != 1 && last_good != 2) ||
        target == last_good || secure_dir(bundle, owner) || secure_dir(txdir, owner) ||
        secure_file(keyfd, owner, 16384)) goto out;
    manifestfd = openat(bundle, "manifest", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    sigfd = openat(bundle, "manifest.sig", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    imagefd = openat(bundle, "rootfs.img", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (manifestfd < 0 || sigfd < 0 || imagefd < 0 ||
        secure_file(manifestfd, owner, sizeof(manifest)) ||
        secure_file(sigfd, owner, sizeof(signature)) ||
        secure_file(imagefd, owner, (off_t)MAX_IMAGE) ||
        read_small(manifestfd, manifest, sizeof(manifest), &manifest_len) ||
        read_small(sigfd, signature, sizeof(signature), &signature_len)) goto out;

    /* Authenticate the bounded manifest before invoking any image parser. */
    keycopy = dup(keyfd);
    if (keycopy < 0 || lseek(keycopy, 0, SEEK_SET) < 0) goto out;
    keyfile = fdopen(keycopy, "r");
    if (!keyfile) goto out;
    keycopy = -1;
    key = PEM_read_PUBKEY(keyfile, NULL, NULL, NULL);
    if (!key || EVP_PKEY_base_id(key) != EVP_PKEY_RSA ||
        EVP_PKEY_bits(key) < 2048) goto out;
    ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestVerifyInit(ctx, NULL, EVP_sha256(), NULL, key) != 1 ||
        EVP_DigestVerifyUpdate(ctx, manifest, manifest_len) != 1 ||
        EVP_DigestVerifyFinal(ctx, signature, signature_len) != 1) goto out;
    if (digest_fd(imagefd, image_hex, &image_size)) goto out;

    rmfd = openat(bundle, "rootfs.manifest", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (rmfd >= 0) {
        have_rm = 1;
        rm_buf = malloc(MAX_ROOTFS_MANIFEST);
        if (!rm_buf || secure_file(rmfd, owner, MAX_ROOTFS_MANIFEST) ||
            read_small(rmfd, rm_buf, MAX_ROOTFS_MANIFEST, &rm_len) ||
            validate_rootfs_manifest_bytes(rm_buf, rm_len) != 0 ||
            digest_fd(rmfd, rm_hex, &rm_size) != 0)
            goto out;
    } else if (errno != ENOENT) {
        goto out;
    }

    if (have_rm && manifest_len >= 13 &&
        memcmp(manifest, "SGW-SYSTEM-3\n", 13) == 0) {
        char mcopy[1025], ver[65] = {0};
        char *ln, *save = NULL;
        uint32_t sys_ver = 0, min_bcap = 0, dmin = 0, dmax = 0;
        int lno = 0;
        if (manifest_len >= sizeof(mcopy)) {
            bfail(err, errlen, "manifest too large");
            goto out;
        }
        memcpy(mcopy, manifest, manifest_len);
        mcopy[manifest_len] = '\0';
        for (ln = strtok_r(mcopy, "\n", &save); ln; ln = strtok_r(NULL, "\n", &save), ++lno) {
            if (lno == 2 && strncmp(ln, "version=", 8) == 0) {
                snprintf(ver, sizeof(ver), "%s", ln + 8);
            } else if (lno == 3) {
                if (parse_u32_line(ln, "sys_version=", &sys_ver) != 0) {
                    bfail(err, errlen, "invalid manifest sys_version");
                    goto out;
                }
            } else if (lno == 4) {
                if (parse_u32_line(ln, "min_boot_cap=", &min_bcap) != 0) {
                    bfail(err, errlen, "invalid manifest min_boot_cap");
                    goto out;
                }
            } else if (lno == 5) {
                if (parse_u32_line(ln, "data_schema_min=", &dmin) != 0) {
                    bfail(err, errlen, "invalid manifest data_schema_min");
                    goto out;
                }
            } else if (lno == 6) {
                if (parse_u32_line(ln, "data_schema_max=", &dmax) != 0) {
                    bfail(err, errlen, "invalid manifest data_schema_max");
                    goto out;
                }
            }
        }
        if (!valid_version_str(ver) || sys_ver == 0 || min_bcap == 0 ||
            dmin == 0 || dmin > dmax) {
            bfail(err, errlen, "invalid SGW-SYSTEM-3 compatibility metadata");
            goto out;
        }
        n = snprintf(canonical, sizeof(canonical),
                     "SGW-SYSTEM-3\nboard=imx6ull-100ask\nversion=%s\n"
                     "sys_version=%u\nmin_boot_cap=%u\n"
                     "data_schema_min=%u\ndata_schema_max=%u\n"
                     "image=rootfs.img\nsize=%llu\nsha256=%s\n"
                     "rootfs_manifest_sha256=%s\n",
                     ver, sys_ver, min_bcap, dmin, dmax,
                     image_size, image_hex, rm_hex);
        if (n <= 0 || (size_t)n != manifest_len || memcmp(canonical, manifest, manifest_len)) {
            bfail(err, errlen, "signed bundle verification failed");
            goto out;
        }
        if (compat) {
            if (min_bcap > compat->boot_cap) {
                bfail(err, errlen, "bootloader capability below package min_boot_cap");
                goto out;
            }
            if (compat->data_schema < dmin || compat->data_schema > dmax) {
                bfail(err, errlen, "DATA schema incompatible with package schema range");
                goto out;
            }
            if (sys_ver < compat->min_sys_version) {
                bfail(err, errlen, "package sys_version below anti-rollback floor");
                goto out;
            }
        }
    } else {
        if (compat && compat->require_compat_manifest) {
            bfail(err, errlen, "manifest missing version/capability/schema compatibility fields");
            goto out;
        }
        if (have_rm) {
            n = snprintf(canonical, sizeof(canonical),
                         "SGW-SYSTEM-2\nboard=imx6ull-100ask\nimage=rootfs.img\nsize=%llu\n"
                         "sha256=%s\nrootfs_manifest_sha256=%s\n",
                         image_size, image_hex, rm_hex);
        } else {
            n = snprintf(canonical, sizeof(canonical),
                         "SGW-SYSTEM-1\nboard=imx6ull-100ask\nimage=rootfs.img\nsize=%llu\nsha256=%s\n",
                         image_size, image_hex);
        }
        if (n <= 0 || (size_t)n != manifest_len || memcmp(canonical, manifest, manifest_len)) {
            bfail(err, errlen, "signed bundle verification failed");
            goto out;
        }
    }
    if (have_rm && verify_manifest_matches_ext4_image(imagefd, rm_buf, rm_len) != 0)
        goto out;
    EVP_MD_CTX_free(ctx); ctx = EVP_MD_CTX_new();
    if (!ctx || EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1 ||
        EVP_DigestUpdate(ctx, manifest, manifest_len) != 1 ||
        EVP_DigestUpdate(ctx, signature, signature_len) != 1 ||
        EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 || digest_len != 32) goto out;
    for (i = 0; i < 32; ++i) {
        package_hex[2*i] = table[digest[i] >> 4];
        package_hex[2*i+1] = table[digest[i] & 15];
    }
    package_hex[64] = '\0';
    lockfd = openat(txdir, ".sgw-transaction.lock",
                    O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (lockfd < 0) goto out;
    {
        struct stat st;
        if (fstat(lockfd, &st) || !S_ISREG(st.st_mode) || st.st_uid != owner ||
            (st.st_mode & 0077) || st.st_nlink != 1) goto out;
    }
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0 || no_active_record(txdir, owner) != 0)
        goto out;
    if (random_id(generated) != 0) goto out;

    if (have_rm) {
        n = snprintf(rm_name, sizeof(rm_name), "txn-%s.manifest", generated);
        if (n <= 0 || (size_t)n >= sizeof(rm_name)) goto out;
        rm_outfd = openat(txdir, rm_name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (rm_outfd < 0) goto out;
        created_rm = 1;
        if (write_all(rm_outfd, (const char *)rm_buf, rm_len) || fsync(rm_outfd)) goto out;
        if (close(rm_outfd) != 0) { rm_outfd = -1; goto out; }
        rm_outfd = -1;
    }

    n = snprintf(name, sizeof(name), "txn-%s", generated);
    if (n <= 0 || (size_t)n >= sizeof(name)) goto out;
    if (have_rm) {
        n = snprintf(record, sizeof(record),
                     "SGW-TXN-1\nstate=VERIFIED\nid=%s\npackage=%s\nimage=%s\n"
                     "rootfs_manifest=%s\ntarget=%u\nlast_good=%u\n",
                     generated, package_hex, image_hex, rm_hex, target, last_good);
    } else {
        n = snprintf(record, sizeof(record),
                     "SGW-TXN-1\nstate=VERIFIED\nid=%s\npackage=%s\nimage=%s\ntarget=%u\nlast_good=%u\n",
                     generated, package_hex, image_hex, target, last_good);
    }
    if (n <= 0 || (size_t)n >= sizeof(record)) goto out;
    recordfd = openat(txdir, name, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (recordfd < 0) goto out;
    created = 1;
    if (write_all(recordfd, record, (size_t)n) || fsync(recordfd)) goto out;
    if (close(recordfd) != 0) { recordfd = -1; goto out; }
    recordfd = -1;
    if (fsync(txdir)) goto out;
    memcpy(id, generated, sizeof(generated));
    rc = 0;
out:
    if (rc != 0 && created) { (void)unlinkat(txdir, name, 0); }
    if (rc != 0 && created_rm) { (void)unlinkat(txdir, rm_name, 0); }
    if (rc != 0 && (created || created_rm)) { (void)fsync(txdir); }
    if (rm_outfd >= 0) close(rm_outfd);
    if (recordfd >= 0) close(recordfd);
    if (lockfd >= 0) { (void)flock(lockfd, LOCK_UN); close(lockfd); }
    EVP_MD_CTX_free(ctx);
    EVP_PKEY_free(key);
    if (keyfile) fclose(keyfile);
    if (keycopy >= 0) close(keycopy);
    if (rmfd >= 0) close(rmfd);
    free(rm_buf);
    if (manifestfd >= 0) close(manifestfd);
    if (sigfd >= 0) close(sigfd);
    if (imagefd >= 0) close(imagefd);
    return rc;
}


int r3c_bundle_record(int bundle, int txdir, int keyfd, uid_t owner,
                      unsigned int target, unsigned int last_good, char id[65])
{
    return r3c_bundle_record_checked(bundle, txdir, keyfd, owner,
                                     target, last_good, NULL, id, NULL, 0);
}
