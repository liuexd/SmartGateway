#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include "r3c_bundle.h"

#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/random.h>
#include <sys/stat.h>
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

static int valid_manifest_relpath(const char *p)
{
    size_t n;
    if (!p || p[0] != '/' || p[1] == '\0') return 0;
    n = strlen(p);
    if (n >= 256 || p[n - 1] == '/' || strstr(p, "//") ||
        strstr(p, "/../") || strstr(p, "/./") || strpbrk(p, " \t\r\n\\"))
        return 0;
    if (n >= 3 && strcmp(p + n - 3, "/..") == 0) return 0;
    if (n >= 2 && strcmp(p + n - 2, "/.") == 0) return 0;
    return 1;
}

static int validate_rootfs_manifest_bytes(const unsigned char *buf, size_t len)
{
    char *copy, *line, *save = NULL;
    int count = 0;
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
        if (!valid_manifest_relpath(line) || !valid_hex64(sp + 1)) {
            free(copy);
            return -1;
        }
        count++;
    }
    free(copy);
    return count > 0 ? 0 : -1;
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

static int no_existing_record(int txdir)
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
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0 &&
            strcmp(entry->d_name, ".sgw-transaction.lock") != 0) goto out;
        errno = 0;
    }
    if (errno == 0) rc = 0;
out:
    closedir(dir);
    return rc;
}

int r3c_bundle_record(int bundle, int txdir, int keyfd, uid_t owner,
                      unsigned int target, unsigned int last_good, char id[65])
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
        read_small(sigfd, signature, sizeof(signature), &signature_len) ||
        digest_fd(imagefd, image_hex, &image_size)) goto out;

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
    if (n <= 0 || (size_t)n != manifest_len || memcmp(canonical, manifest, manifest_len)) goto out;

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
    if (flock(lockfd, LOCK_EX | LOCK_NB) != 0 || no_existing_record(txdir) != 0)
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
