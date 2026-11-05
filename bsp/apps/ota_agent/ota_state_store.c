
/*
 * ota_state_store.c - OTA 状态文件持久化工具
 *
 * 用法: ota_state_store <目录> <STATE> <CONFIRMED_VERSION> <CANDIDATE_VERSION>
 * 作用: 生成/覆盖 <目录>/ota_state.json，记录当前 OTA 状态机状态。
 *       采用"临时文件 + fsync + rename + 目录 fsync"的原子写入流程，
 *       即使写入过程中掉电，也不会留下半截损坏的状态文件。
 * 输出: 成功时打印 PASS 并返回 0；任何一步失败返回 1。
 */

/* 启用 POSIX.1-2008 接口（mkstemp、fsync、O_DIRECTORY 等） */
#define _POSIX_C_SOURCE 200809L

/* 标准库头文件 */
#include <errno.h>  /* errno、EINTR，用于处理被信号打断的 write */
#include <fcntl.h>  /* open、O_RDONLY、O_DIRECTORY */
#include <limits.h> /* PATH_MAX，路径缓冲区长度上限 */
#include <stdio.h>  /* fprintf、printf、snprintf、perror */
#include <stdlib.h> /* mkstemp、unlink */
#include <string.h> /* strlen */
#include <unistd.h> /* write、fsync、close */

#include <json-c/json.h> /* 构造 JSON 状态文件 */

/*
 * write_all - 把 length 字节全部写入 fd，处理"短写"和 EINTR
 *
 * fd     : 已打开的可写文件描述符
 * data   : 数据起始地址
 * length : 需要写入的总字节数
 *
 * 返回 0 表示全部写完，-1 表示写入失败
 *
 * 为什么要循环: write 可能只写入一部分（短写），必须循环直到 total == length；
 *               否则状态文件会被截断成"看起来合法但缺字段"的内容。
 */
static int write_all(int fd, const char *data, size_t length)
{
    size_t total = 0; /* 已成功写入的字节数 */

    while (total < length) {
        /* 从已写入位置继续写剩余部分 */
        ssize_t n = write(fd, data + total, length - total);

        /* 被信号打断不算失败，重试即可 */
        if (n < 0 && errno == EINTR)
            continue;

        /* n < 0 是真实错误；n == 0 表示写不动了，无法继续推进 */
        if (n <= 0)
            return -1;

        total += (size_t)n;
    }

    return 0;
}

/*
 * main - OTA 状态持久化入口
 *
 * argv[1] : 目标目录（状态文件写在这里）
 * argv[2] : state，状态机当前阶段，如 "idle"/"pending"/"confirmed"
 * argv[3] : confirmed_version，已确认、可回滚的版本
 * argv[4] : candidate_version，本次待验证的新版本
 *
 * 写入流程（保证原子性:）:
 *   1. mkstemp 在同一目录创建唯一临时文件
 *   2. 完整写入新内容并补换行
 *   3. fsync 临时文件，确保数据真正落盘
 *   4. rename 原子替换正式文件（同一文件系统内 rename 是原子的）
 *   5. fsync 目录，确保 rename 这个目录项变更本身也落盘
 *
 * 返回 0 表示成功，1 表示失败
 */
int main(int argc, char **argv)
{
    char temp_path[PATH_MAX];  /* 临时文件路径，mkstemp 会就地改写 XXXXXX 后缀 */
    char state_path[PATH_MAX]; /* 正式状态文件路径 ota_state.json */

    json_object *root = NULL; /* 待写入的 JSON 对象树 */

    int fd = -1;          /* 临时文件的 fd，-1 表示未打开 */
    int dirfd = -1;       /* 目录 fd，用于 fsync 目录元数据 */
    int temp_created = 0; /* 标记临时文件是否还存在，决定 cleanup 是否要 unlink */
    int result = 1;       /* 默认失败，全部成功后才置 0 */

    /* 参数个数检查: 程序名 + 4 个参数 */
    if (argc != 5) {
        fprintf(stderr,
                "Usage: %s DIR STATE CONFIRMED CANDIDATE\n",
                argv[0]);
        return 1;
    }

    const char *directory = argv[1];

    /* 拼接两条路径，并用返回值判断是否被截断
     * （snprintf 返回的是"本应写入的长度"，>= 缓冲区大小即说明装不下）
     */
    int a = snprintf(temp_path, sizeof(temp_path),
                     "%s/ota_state.json.tmp.XXXXXX", directory);

    int b = snprintf(state_path, sizeof(state_path),
                     "%s/ota_state.json", directory);

    if (a < 0 || (size_t)a >= sizeof(temp_path) ||
        b < 0 || (size_t)b >= sizeof(state_path)) {
        fprintf(stderr, "Path too long\n");
        return 1;
    }

    /* 构造 JSON 对象树:
     *   schema_version    固定为 1，供读取方做格式兼容判断
     *   state             状态机当前阶段
     *   confirmed_version 已确认、可回滚的版本
     *   candidate_version 本次待验证的候选版本
     * json_object_object_add 会接管传入对象的引用计数，因此无需单独释放子对象。
     */
    root = json_object_new_object();
    if (!root)
        return 1;

    json_object_object_add(root, "schema_version",
                           json_object_new_int(1));

    json_object_object_add(root, "state",
                           json_object_new_string(argv[2]));

    json_object_object_add(root, "confirmed_version",
                           json_object_new_string(argv[3]));

    json_object_object_add(root, "candidate_version",
                           json_object_new_string(argv[4]));

    /* 序列化为带缩进的 JSON 文本，便于人工查看。
     * 返回的指针由 root 内部持有，不能自行释放，也不能在 root 释放后继续使用。
     */
    const char *data = json_object_to_json_string_ext(
        root, JSON_C_TO_STRING_PRETTY);

    if (!data)
        goto cleanup;

    /* 第一步：创建唯一的临时文件
     * mkstemp 把路径里的 XXXXXX 就地替换成随机字符（后续 unlink/rename 用的就是真实文件名），
     * 同时返回一个以 O_RDWR 打开、权限 0600 的 fd；若同名文件已存在则不会复用。
     */
    fd = mkstemp(temp_path);
    if (fd < 0) {
        perror("mkstemp");
        goto cleanup;
    }
    temp_created = 1;

    /* 第二步：完整写入新状态（JSON 正文 + 结尾换行） */
    if (write_all(fd, data, strlen(data)) != 0 ||
        write_all(fd, "\n", 1) != 0) {
        perror("write");
        goto cleanup;
    }

    /* 第三步：同步临时文件内容
     * 只有 fsync 成功，才能保证数据真正写入存储介质；
     * 否则 rename 之后可能指向"内容为空/残缺"的文件（掉电场景）。
     */
    if (fsync(fd) != 0) {
        perror("fsync file");
        goto cleanup;
    }

    /* 关闭临时文件，close 失败同样视为写入失败 */
    if (close(fd) != 0) {
        fd = -1;
        perror("close");
        goto cleanup;
    }
    fd = -1;

    /* 第四步：将临时文件原子替换为正式状态文件
     * 同文件系统内 rename 是原子操作: 读者要么看到旧文件、要么看到完整的新文件，
     * 不会看到"写了一半"的中间状态。
     */
    if (rename(temp_path, state_path) != 0) {
        perror("rename");
        goto cleanup;
    }
    /* 临时文件已被 rename 消费掉，cleanup 时不要再去 unlink */
    temp_created = 0;

    /* 第五步：同步目录元数据
     * rename 改变的是目录项，只有 fsync 目录才能确保这个"文件名指向变更"落盘；
     * 缺了这一步，掉电后可能回滚到旧文件名或找不到状态文件。
     */
    dirfd = open(directory, O_RDONLY | O_DIRECTORY);

    if (dirfd < 0 || fsync(dirfd) != 0) {
        perror("fsync directory");
        goto cleanup;
    }

    printf("PASS: state persisted to %s\n", state_path);
    result = 0;

cleanup:
    /* 统一清理: 关闭可能仍打开的 fd */
    if (fd >= 0)
        close(fd);

    if (dirfd >= 0)
        close(dirfd);

    /* 失败路径上若临时文件还残留，主动删除避免污染目标目录 */
    if (temp_created)
        unlink(temp_path);

    /* 释放 JSON 对象树（会连带释放所有子对象与序列化缓冲区） */
    json_object_put(root);
    return result;
}
