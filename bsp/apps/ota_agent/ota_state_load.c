
/*
 * ota_state_load.c - OTA 状态文件读取与校验工具
 *
 * 用法: ota_state_load <state-file>
 * 作用: 读取 ota_state_store 写下的 ota_state.json，做安全校验并给出恢复建议
 *       1. 以只读 + 不跟随符号链接方式打开，限制文件类型与大小
 *       2. 严格模式（JSON_TOKENER_STRICT）解析，且必须完整消费整个文件
 *       3. 校验 schema_version == 1、三个字符串字段合法、state 值属于已知状态机
 *       4. 根据当前 state 输出一行 RECOVERY 建议，供上层决定后续动作
 * 返回值: 0 表示读取并校验成功，非 0 表示失败
 */

/* 启用 POSIX.1-2008 接口（O_CLOEXEC、O_NOFOLLOW 等） */
#define _POSIX_C_SOURCE 200809L

/* 标准库头文件 */
#include <errno.h>    /* errno、EINTR，用于处理被信号打断的 read */
#include <fcntl.h>    /* open、O_RDONLY、O_CLOEXEC、O_NOFOLLOW */
#include <stdio.h>    /* fprintf、printf、perror */
#include <string.h>   /* strcmp、strlen */
#include <sys/stat.h> /* fstat、struct stat、S_ISREG */
#include <unistd.h>   /* read、close */

#include <json-c/json.h> /* JSON 解析 */

/* 状态文件大小上限（字节）: 正常状态文件只有几百字节，4 KiB 足够，
 * 同时也是一次性读入内存的缓冲区大小（另加 1 字节放 '\0'）。 */
#define STATE_FILE_MAX 4096

/*
 * get_string_field - 取出一个字符串字段并校验内容长度
 *
 * root  : 已解析的状态文件根对象
 * name  : 字段名
 * value : 输出参数，成功时指向 JSON 内部的 C 字符串（由 root 持有，不要释放）
 *
 * 校验点: 字段存在、类型为 string、非空、长度 < 64
 * 返回 0 成功，-1 失败
 */
static int get_string_field(
    json_object *root,
    const char *name,
    const char **value)
{
    json_object *field = NULL;

    /* 字段缺失或类型不是字符串，直接判定非法 */
    if (!json_object_object_get_ex(root, name, &field) ||
        json_object_get_type(field) != json_type_string) {
        fprintf(stderr, "FAIL: invalid field %s\n", name);
        return -1;
    }

    /* json_object_get_string 返回 JSON 对象内部缓冲区，生命周期跟着 root */
    *value = json_object_get_string(field);

    /* 拒绝空串和过长串: 长度上限 64 是版本号/状态名的合理范围，
     * 防止异常内容进入后续的路径拼接或比较逻辑 */
    if (*value == NULL || (*value)[0] == '\0' ||
        strlen(*value) >= 64) {
        fprintf(stderr, "FAIL: invalid value %s\n", name);
        return -1;
    }

    return 0;
}

/*
 * valid_state - 判断状态字符串是否属于已知的 OTA 状态机取值
 *
 * 白名单式校验: 只接受下面列出的 9 个状态，其余一律拒绝，
 * 避免上层拿到无法处理的状态字符串。
 * 返回 1 表示合法，0 表示非法
 */
static int valid_state(const char *state)
{
    /* OTA 状态机的完整状态集合，顺序与正常流转过程一致 */
    static const char *allowed[] = {
        "IDLE",              /* 空闲，无升级任务 */
        "DOWNLOADED",        /* 升级包已下载 */
        "VERIFIED",          /* 升级包已通过校验 */
        "STAGED",            /* 已暂存到待安装位置 */
        "ACTIVATED",         /* 已切换并启动新版本，待观察 */
        "HEALTHY",           /* 新版本自检通过 */
        "CONFIRMED",         /* 新版本已正式确认，不再回滚 */
        "ROLLBACK_PENDING",  /* 需回滚，尚未执行 */
        "ROLLED_BACK"        /* 已回滚到旧版本 */
    };

    /* 线性遍历查找: 元素很少，无需哈希或二分 */
    for (size_t i = 0;
         i < sizeof(allowed) / sizeof(allowed[0]);
         i++) {
        if (strcmp(state, allowed[i]) == 0)
            return 1;
    }

    return 0;
}

/*
 * main - 读取并校验 OTA 状态文件的入口
 *
 * argv[1] 为状态文件路径（通常是 <目录>/ota_state.json）
 *
 * 校验顺序: 打开文件 -> 文件类型/大小 -> 完整读入缓冲 -> 严格解析 JSON
 *           -> 字段与状态机校验 -> 打印恢复建议
 * 任意一步失败都跳到 cleanup 并返回 1
 */
int main(int argc, char **argv)
{
    char buffer[STATE_FILE_MAX + 1]; /* 状态文件全文 + 结尾 '\0' */
    struct stat st;                  /* fstat 得到的文件属性 */

    json_tokener *tok = NULL; /* json-c 的解析器实例，可获取详细错误位置 */
    json_object *root = NULL; /* 解析出的根对象 */
    json_object *schema = NULL; /* schema_version 字段 */

    const char *state = NULL;     /* 状态机当前阶段 */
    const char *confirmed = NULL; /* 已确认、可回滚的版本 */
    const char *candidate = NULL; /* 待验证的候选版本 */

    int fd = -1;           /* 状态文件描述符 */
    int result = 1;        /* 默认失败，全部校验通过后置 0 */
    size_t total = 0;      /* 已读入的字节数 */

    /* 步骤 1: 参数检查，只接受一个状态文件路径 */
    if (argc != 2) {
        fprintf(stderr, "Usage: %s STATE_FILE\n", argv[0]);
        return 1;
    }

    /* 步骤 2: 只读打开状态文件
     *   O_RDONLY  : 只读，绝不修改状态文件
     *   O_CLOEXEC : exec 时自动关闭，避免 fd 泄漏到子进程
     *   O_NOFOLLOW: 拒绝符号链接，防止被诱导去读任意文件
     */
    fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0) {
        perror("open");
        goto cleanup;
    }

    /* 步骤 3: 校验文件类型与大小
     *   必须是普通文件、非空、不超过 STATE_FILE_MAX
     *   上限检查很关键: 下面用固定大小的栈缓冲区一次性读入
     */
    if (fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size <= 0 ||
        st.st_size > STATE_FILE_MAX) {
        fprintf(stderr, "FAIL: invalid state file size/type\n");
        goto cleanup;
    }

    /* 步骤 4: 循环读到缓冲区
     *   read 可能短读或被信号打断，因此必须循环到 total == st.st_size
     *   若中途 n == 0，说明文件在读取过程中被截短，视为错误
     */
    while (total < (size_t)st.st_size) {
        ssize_t n = read(
            fd,
            buffer + total,
            (size_t)st.st_size - total);

        /* 被信号打断时重试 */
        if (n < 0 && errno == EINTR)
            continue;

        if (n <= 0) {
            fprintf(stderr, "FAIL: state file read error\n");
            goto cleanup;
        }

        total += (size_t)n;
    }

    /* 补上 C 字符串结束符，便于交给解析器处理 */
    buffer[total] = '\0';

    /* 步骤 5: 创建解析器
     * 用显式的 json_tokener 而不是 json_object_from_file，
     * 是为了能拿到解析错误类型和已消费字节数（见下一步）。
     */
    tok = json_tokener_new();

    if (tok == NULL)
        goto cleanup;

    /* 严格模式: 拒绝单引号字符串、尾随逗号、注释等非标准 JSON 写法 */
    json_tokener_set_flags(tok, JSON_TOKENER_STRICT);

    /* 步骤 6: 解析缓冲区内容 */
    root = json_tokener_parse_ex(
        tok, buffer, (int)total);

    /* 四个条件全部满足才算合法:
     *   1) 解析无错误
     *   2) 得到了根对象
     *   3) 根节点是对象（不是数组/字符串）
     *   4) 消费的字节数正好等于文件长度（防止文件末尾拼了额外垃圾内容）
     */
    if (json_tokener_get_error(tok) != json_tokener_success ||
        root == NULL ||
        json_object_get_type(root) != json_type_object ||
        (size_t)json_tokener_get_parse_end(tok) != total) {
        fprintf(stderr, "FAIL: invalid state JSON\n");
        goto cleanup;
    }

    /* 步骤 7: 字段与状态机校验
     *   - schema_version 必须存在、是整数、等于 1
     *   - state/confirmed_version/candidate_version 三个字符串字段合法
     *   - state 必须属于已知状态机取值（白名单）
     */
    if (!json_object_object_get_ex(root, "schema_version",
                                   &schema) ||
        json_object_get_type(schema) != json_type_int ||
        json_object_get_int(schema) != 1 ||
        get_string_field(root, "state", &state) != 0 ||
        get_string_field(root, "confirmed_version",
                         &confirmed) != 0 ||
        get_string_field(root, "candidate_version",
                         &candidate) != 0 ||
        !valid_state(state)) {
        fprintf(stderr, "FAIL: invalid state metadata\n");
        goto cleanup;
    }

    /* 校验通过，回显文件内容 */
    printf("PASS: state file loaded\n");
    printf("State     : %s\n", state);
    printf("Confirmed : %s\n", confirmed);
    printf("Candidate : %s\n", candidate);

    /*
     * 当前只给出恢复建议，不执行实际回滚。
     * 后续还必须检查 current 软链接及真实安装目录。
     *
     * 分支含义:
     *   ACTIVATED / HEALTHY / ROLLBACK_PENDING
     *       -> 新版本已启动但尚未定性，或已判定需要回滚；
     *          需要先核对实际运行的版本，再决定是否回滚
     *   CONFIRMED
     *       -> 已正式确认，只需校验确认版本与实际安装是否一致
     *   其余（IDLE / DOWNLOADED / VERIFIED / STAGED / ROLLED_BACK）
     *       -> 升级未进入"已切换"阶段，只需对齐记录与实际当前版本
     */
    if (strcmp(state, "ACTIVATED") == 0 ||
        strcmp(state, "HEALTHY") == 0 ||
        strcmp(state, "ROLLBACK_PENDING") == 0) {
        printf("RECOVERY: RECONCILE_AND_CHECK_ROLLBACK\n");
    } else if (strcmp(state, "CONFIRMED") == 0) {
        printf("RECOVERY: VERIFY_CONFIRMED_VERSION\n");
    } else {
        printf("RECOVERY: RECONCILE_CURRENT_VERSION\n");
    }

    result = 0;

cleanup:
    /* 统一清理: 关闭 fd -> 释放 JSON 对象树 -> 释放解析器。
     * 顺序上先释放 root 再释放 tok（root 可能引用 tok 内部内容）。
     */
    if (fd >= 0)
        close(fd);

    if (root != NULL)
        json_object_put(root);

    if (tok != NULL)
        json_tokener_free(tok);

    return result;
}
