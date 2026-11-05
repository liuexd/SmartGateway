
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LINK_NAME "current"
#define TEMP_NAME "current.next"

int main(int argc, char **argv)
{
    int dirfd = -1;
    int temp_created = 0;
    int result = 1;

    char target[128];
    char previous[128];

    struct stat st;

    if (argc != 3) {
        fprintf(stderr,
                "Usage: %s LAB_DIR VERSION\n",
                argv[0]);
        return 1;
    }

    /* 实验阶段只允许切换两个固定版本 */
    if (strcmp(argv[2], "v0.1.0-test1") != 0 &&
        strcmp(argv[2], "v0.1.1-test1") != 0) {
        fprintf(stderr, "FAIL: unsupported version\n");
        return 1;
    }

    int n = snprintf(target, sizeof(target),
                     "releases/%s", argv[2]);

    if (n < 0 || (size_t)n >= sizeof(target))
        return 1;

    /* 打开版本管理目录 */
    dirfd = open(argv[1],
                 O_RDONLY | O_DIRECTORY |
                 O_CLOEXEC | O_NOFOLLOW);

    if (dirfd < 0) {
        perror("open directory");
        return 1;
    }

    /* 确认目标版本目录存在且不是符号链接。
     *
     * 为什么用 fstatat 而不是 fstat:
     *   fstat 只能作用于"已打开的 fd"，且 fd 在打开时路径就已解析完毕，
     *   无法再指定"是否跟随符号链接"；
     *   fstatat 按 "dirfd + 相对路径" 解析，可以用 AT_SYMLINK_NOFOLLOW
     *   取得 lstat 语义（即检查该目录项本身，而不是它指向的东西）。
     *
     * 三个条件各自的作用:
     *   1) fstatat(...) != 0  -> 目标版本目录不存在，切过去会导致 current 打不开
     *   2) !S_ISDIR(...)      -> 必须是目录；current 要作为目录前缀用
     *                            (current/smart_gateway)，指向普通文件没有意义
     *   3) AT_SYMLINK_NOFOLLOW -> 目标自身不能是符号链接。若不加此 flag，
     *                            fstatat 会跟随软链，那么 releases/vX -> /etc
     *                            这类链接也能通过检查，current 就会变成
     *                            current -> releases/vX -> /etc 的多级跳转，
     *                            越过版本目录边界。加了之后，软链的 lstat
     *                            结果为 S_ISDIR 为假，直接被拒绝。
     *
     * 注意: AT_SYMLINK_NOFOLLOW 只作用于路径最后一段；target 中间的
     *       "releases/" 段仍会被正常跟随。
     *
     * 另外 dirfd 是以 O_DIRECTORY | O_NOFOLLOW 打开的，等于把实验室目录"钉住"；
     * 后续的 fstatat / symlinkat / renameat 全部相对它执行，
     * 即使 argv[1] 这个路径之后被 rename 或替换成软链，这些操作也不会跑偏。
     */
    if (fstatat(dirfd, target, &st,
                AT_SYMLINK_NOFOLLOW) != 0 ||
        !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "FAIL: target release unavailable\n");
        goto cleanup;
    }

    /* 读取当前版本 */
    ssize_t len = readlinkat(
        dirfd, LINK_NAME,
        previous, sizeof(previous) - 1);

    if (len < 0) {
        perror("readlinkat");
        goto cleanup;
    }

    previous[len] = '\0';

    printf("Before: %s\n", previous);

    /* 先创建指向新版本的临时软链接
     * current.next -> dirfd/target
     */
    if (symlinkat(target, dirfd, TEMP_NAME) != 0) {
        perror("symlinkat");
        goto cleanup;
    }

    temp_created = 1;

    /* 原子替换正式的 current 软链接 */
    if (renameat(dirfd, TEMP_NAME,
                 dirfd, LINK_NAME) != 0) {
        perror("renameat");
        goto cleanup;
    }

    temp_created = 0;

    /*
     * 将目录项更新请求同步到存储设备。
     * 如果此处失败，切换可能已发生，
     * 不能直接认为旧版本仍然有效。
     */
    if (fsync(dirfd) != 0) {
        perror("fsync directory");
        goto cleanup;
    }

    len = readlinkat(
        dirfd, LINK_NAME,
        previous, sizeof(previous) - 1);

    if (len < 0) {
        perror("readlinkat after switch");
        goto cleanup;
    }

    previous[len] = '\0';

    printf("After : %s\n", previous);
    printf("PASS: atomic switch completed\n");

    result = 0;

cleanup:
    if (temp_created)
        unlinkat(dirfd, TEMP_NAME, 0);

    if (dirfd >= 0)
        close(dirfd);

    return result;
}
