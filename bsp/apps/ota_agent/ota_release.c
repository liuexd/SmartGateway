
#define _GNU_SOURCE

#include "ota_release.h"
#include "ota_verify.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <linux/fs.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <json-c/json.h>

#define OTA_VERSION_MAX 64
#define OTA_MANIFEST_MAX 4096
#define OTA_COPY_MAX (64 * 1024 * 1024)

/* 从 Manifest 读取规范版本号 */
static int load_package_version(
    int pkgfd,
    char version[OTA_VERSION_MAX])
{
    char buf[OTA_MANIFEST_MAX + 1];
    struct stat st;
    json_tokener *tok = NULL;
    json_object *root = NULL;
    json_object *field = NULL;
    int fd = -1;
    int result = -1;
    size_t done = 0;

    fd = openat(pkgfd, "manifest.json",
                O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        goto out;

    if (fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        st.st_size > OTA_MANIFEST_MAX)
        goto out;

    while (done < (size_t)st.st_size) {
        ssize_t n = read(fd, buf + done,
                         (size_t)st.st_size - done);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            goto out;
        done += (size_t)n;
    }

    buf[done] = '\0';

    tok = json_tokener_new();
    if (!tok)
        goto out;

    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);
    root = json_tokener_parse_ex(tok, buf, (int)done);

    if (json_tokener_get_error(tok) != json_tokener_success ||
        !root ||
        json_object_get_type(root) != json_type_object ||
        (size_t)json_tokener_get_parse_end(tok) != done)
        goto out;

    if (!json_object_object_get_ex(root, "version", &field) ||
        json_object_get_type(field) != json_type_string)
        goto out;

    const char *s = json_object_get_string(field);
    size_t len = (size_t)json_object_get_string_len(field);

    if (!s || len == 0 || len >= OTA_VERSION_MAX ||
        strlen(s) != len)
        goto out;

    /* 规范版本号必须以数字开头，不能包含路径字符 */
    if (s[0] < '0' || s[0] > '9')
        goto out;

    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') ||
              (c >= 'A' && c <= 'Z') ||
              (c >= 'a' && c <= 'z') ||
              c == '.' || c == '-' || c == '_'))
            goto out;
    }

    memcpy(version, s, len + 1);
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

/* 复制普通文件，保留短读、短写和 EINTR 处理 */
static int copy_file(
    int srcdir,
    int dstdir,
    const char *name,
    mode_t mode)
{
    char buf[16384];
    struct stat st;
    int src = -1;
    int dst = -1;
    int result = -1;
    size_t total = 0;

    src = openat(srcdir, name,
                 O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (src < 0)
        goto out;

    if (fstat(src, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        st.st_size > OTA_COPY_MAX)
        goto out;

    dst = openat(dstdir, name,
                 O_WRONLY | O_CREAT | O_EXCL |
                 O_CLOEXEC | O_NOFOLLOW, 0600);
    if (dst < 0)
        goto out;

    for (;;) {
        ssize_t n = read(src, buf, sizeof(buf));

        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0)
            goto out;
        if (n == 0)
            break;

        total += (size_t)n;
        if (total > OTA_COPY_MAX)
            goto out;

        size_t written = 0;
        while (written < (size_t)n) {
            ssize_t w = write(
                dst, buf + written, (size_t)n - written);

            if (w < 0 && errno == EINTR)
                continue;
            if (w <= 0)
                goto out;

            written += (size_t)w;
        }
    }

    if (fchmod(dst, mode) != 0 ||
        fsync(dst) != 0)
        goto out;

    if (close(dst) != 0) {
        dst = -1;
        goto out;
    }
    dst = -1;

    result = 0;

out:
    if (src >= 0)
        close(src);
    if (dst >= 0)
        close(dst);

    if (result != 0)
        unlinkat(dstdir, name, 0);

    return result;
}

int ota_release_stage(
    const char *package_dir,
    const char *install_dir)
{
    char version[OTA_VERSION_MAX];
    char release_name[OTA_VERSION_MAX + 2];
    char temp_path[PATH_MAX];
    char *temp_name = NULL;

    int pkgfd = -1;
    int rootfd = -1;
    int releasesfd = -1;
    int tempfd = -1;
    int temp_created = 0;
    int published = 0;
    int result = -1;

    if (!package_dir || !install_dir)
        return -1;

    /* 校验源升级包 */
    if (ota_verify_package(package_dir) != 0) {
        fprintf(stderr, "FAIL: source verification\n");
        return -1;
    }

    pkgfd = open(package_dir,
                 O_RDONLY | O_DIRECTORY |
                 O_NOFOLLOW | O_CLOEXEC);

    rootfd = open(install_dir,
                  O_RDONLY | O_DIRECTORY |
                  O_NOFOLLOW | O_CLOEXEC);

    if (pkgfd < 0 || rootfd < 0)
        goto out;

    releasesfd = openat(rootfd, "releases",
                        O_RDONLY | O_DIRECTORY |
                        O_NOFOLLOW | O_CLOEXEC);

    if (releasesfd < 0)
        goto out;

    if (load_package_version(pkgfd, version) != 0) {
        fprintf(stderr, "FAIL: invalid package version\n");
        goto out;
    }

    int n = snprintf(release_name, sizeof(release_name),
                     "v%s", version);

    if (n < 0 || (size_t)n >= sizeof(release_name))
        goto out;

    n = snprintf(temp_path, sizeof(temp_path),
                 "%s/releases/.ota-stage-XXXXXX",
                 install_dir);

    if (n < 0 || (size_t)n >= sizeof(temp_path))
        goto out;

    if (!mkdtemp(temp_path))
        goto out;

    temp_created = 1;
    temp_name = strrchr(temp_path, '/');
    if (!temp_name)
        goto out;
    temp_name++;

    tempfd = openat(releasesfd, temp_name,
                    O_RDONLY | O_DIRECTORY |
                    O_NOFOLLOW | O_CLOEXEC);
    if (tempfd < 0)
        goto out;

    if (copy_file(pkgfd, tempfd,
                  "smart_gateway", 0755) != 0 ||
        copy_file(pkgfd, tempfd,
                  "manifest.json", 0644) != 0) {
        fprintf(stderr, "FAIL: copying candidate release\n");
        goto out;
    }

    /* 验证复制后的内容，而不是只信任原始文件 */
    if (ota_verify_package(temp_path) != 0) {
        fprintf(stderr, "FAIL: staged release verification\n");
        goto out;
    }

    if (fchmod(tempfd, 0755) != 0 ||
        fsync(tempfd) != 0)
        goto out;

    /*
     * 只允许创建新版本目录。
     * RENAME_NOREPLACE 防止意外覆盖已安装版本。
     * 若内核或文件系统不支持，直接失败，不降级覆盖。
     */
    if (syscall(SYS_renameat2,
                releasesfd, temp_name,
                releasesfd, release_name,
                RENAME_NOREPLACE) != 0) {
        fprintf(stderr, "FAIL: publish release: %s\n",
                strerror(errno));
        goto out;
    }

    published = 1;

    /* 持久化 release 目录项变更 */
    if (fsync(releasesfd) != 0) {
        fprintf(stderr,
                "FAIL: release directory fsync; "
                "publication may already be visible\n");
        goto out;
    }

    printf("PASS: release installed: releases/%s\n",
           release_name);
    result = 0;

out:
    /*
     * 仅清理尚未发布的临时目录。
     * 已发布目录绝不在失败路径中随意删除。
     */
    if (temp_created && !published && tempfd >= 0) {
        unlinkat(tempfd, "smart_gateway", 0);
        unlinkat(tempfd, "manifest.json", 0);
    }

    if (tempfd >= 0)
        close(tempfd);

    if (temp_created && !published && releasesfd >= 0)
        unlinkat(releasesfd, temp_name, AT_REMOVEDIR);

    if (releasesfd >= 0)
        close(releasesfd);
    if (rootfd >= 0)
        close(rootfd);
    if (pkgfd >= 0)
        close(pkgfd);

    return result;
}
