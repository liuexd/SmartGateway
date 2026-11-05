
/*
 * ota_verify.c - OTA 升级包校验工具
 *
 * 用法: ota_verify <package-directory>
 * 作用: 对 OTA 升级包目录做完整校验，任何一项不通过都直接失败退出
 *       1. 解析 manifest.json，检查元数据（格式版本/应用名/版本号/架构/文件名/大小/SHA-256）
 *       2. 打开包内二进制文件，做文件类型、大小、ELF 头（ARM 32 位小端、非 soft-float）校验
 *       3. 计算文件 SHA-256，与 manifest 中声明的值比对
 * 返回值: 0 表示校验通过，非 0 表示失败
 */

/* 启用 POSIX.1-2008 接口（pread、O_CLOEXEC、O_NOFOLLOW 等） */
#define _POSIX_C_SOURCE 200809L

/* 标准库头文件 */
#include <elf.h>      /* Elf32_Ehdr、EM_ARM、ELFMAG 等 ELF 结构与常量 */
#include <errno.h>    /* errno、EINTR，用于处理被信号打断的 read */
#include <fcntl.h>    /* open、O_RDONLY、O_CLOEXEC、O_NOFOLLOW */
#include <stdint.h>   /* int64_t 等定长整型 */
#include <stdio.h>    /* printf、fprintf、snprintf、perror */
#include <string.h>   /* memcmp、strcmp、strlen */
#include <sys/stat.h> /* fstat、struct stat、S_ISREG */
#include <unistd.h>   /* read、pread、lseek、close */
#include <limits.h>   /* PATH_MAX，路径缓冲区长度上限 */

/* 第三方库: JSON 解析 与 OpenSSL 摘要计算 */
#include <json-c/json.h>
#include <openssl/evp.h>

#include "ota_verify.h"

/* 本阶段只允许升级这一种应用（应用名同时用于匹配 manifest 与二进制文件名） */
#define OTA_APP_NAME "smart_gateway"
/* 只接受该目标架构的升级包 */
#if defined(OTA_TARGET_X86_64)
#define OTA_ARCH "x86_64-linux-gnu"
#else
#define OTA_ARCH "armv7-linux-gnueabihf"
#endif

/* 测试阶段设置最大二进制大小为 50 MiB，避免异常大的包占满存储 */
#define OTA_MAX_SIZE (50LL * 1024 * 1024)

/*
 * get_field - 从 JSON 对象中取出指定名字的字段，并校验其类型
 *
 * root          : 已解析的 manifest 根对象
 * name          : 字段名
 * expected_type : 期望的 JSON 类型，取值来自 json-c 的 enum json_type:
 *                   json_type_null / json_type_boolean / json_type_double /
 *                   json_type_int  / json_type_object  / json_type_array /
 *                   json_type_string
 *                 本文件只用到两种:
 *                   json_type_int    -> format_version、size（必须是整数，
 *                                       写成 1.0 会被判为 double 而失败）
 *                   json_type_string -> application、version、architecture、
 *                                       binary、sha256
 * result        : 输出参数，成功时指向该字段对象（由 root 持有，不要单独释放）
 *
 * 返回 0 表示字段存在且类型正确；返回 -1 表示字段缺失或类型不符
 *
 * 注意: json_type_int 在 json-c 内部以 int64_t 存储，因此 size 字段
 *       可以安全承载 50 MiB 这类数量级。
 */
static int get_field(
    json_object *root,
    const char *name,
    enum json_type expected_type,
    json_object **result)
{
    /* 两个条件用 || 短路:
     *   1) json_object_object_get_ex 查找字段，字段不存在时返回 0（此时 result 不被赋值，
     *      靠短路避免下面解引用它）
     *   2) json_object_get_type(*result) 取出该值的实际类型，必须与 expected_type
     *      完全相等 —— 这里是"等于某一种类型"，不是"属于某几种类型"。
     *      例如 expected_type 为 json_type_int 时，1.0(double)、"1"(string)、
     *      true(boolean)、null(json_type_null) 全部不通过。
     *      字段值为 JSON null 时 get_ex 返回 1 但 *result 为 NULL，
     *      json_object_get_type(NULL) 返回 json_type_null，同样不会通过。
     */
    if (!json_object_object_get_ex(root, name, result) ||
        json_object_get_type(*result) != expected_type)
    {
        /* 统一打印出错字段名，便于定位是哪个字段有问题 */
        fprintf(stderr, "FAIL: invalid field: %s\n", name);
        return -1;
    }

    return 0;
}

/*
 * check_elf - 校验文件是否是本目标平台可用的 ELF 二进制
 *
 * 检查点:
 *   - 文件至少包含完整的 ELF32 文件头
 *   - 魔数、位宽（32 位）、字节序（小端）、ELF 版本
 *   - 目标机器为 ARM
 *   - 未显式标记为 soft-float ABI（与目标板的硬浮点环境匹配）
 *
 * 返回 0 表示通过，-1 表示不是受支持的 ARM ELF
 */


static int check_elf(int fd)
{
#if defined(OTA_TARGET_X86_64)
    /* Ubuntu x86-64 测试版本 */
    Elf64_Ehdr header;
#else
    /* i.MX6ULL ARMv7 版本 */
    Elf32_Ehdr header;
#endif

    ssize_t n = pread(fd, &header, sizeof(header), 0);

    if (n != (ssize_t)sizeof(header))
    {
        fprintf(stderr, "FAIL: ELF header incomplete\n");
        return -1;
    }

    /* 通用 ELF 检查 */
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB ||
        header.e_ident[EI_VERSION] != EV_CURRENT ||
        header.e_version != EV_CURRENT)
    {
        fprintf(stderr, "FAIL: invalid ELF header\n");
        return -1;
    }

#if defined(OTA_TARGET_X86_64)

    /* x86-64 必须是 ELF64 + EM_X86_64 */
    if (header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_machine != EM_X86_64)
    {
        fprintf(stderr, "FAIL: not a supported x86-64 ELF\n");
        return -1;
    }

#else

    /* i.MX6ULL 必须是 ELF32 + EM_ARM */
    if (header.e_ident[EI_CLASS] != ELFCLASS32 ||
        header.e_machine != EM_ARM)
    {
        fprintf(stderr, "FAIL: not a supported ARM ELF\n");
        return -1;
    }

    /* 拒绝显式标记为 soft-float 的 ARM ELF */
    if ((header.e_flags & EF_ARM_ABI_FLOAT_SOFT) != 0)
    {
        fprintf(stderr, "FAIL: soft-float ABI rejected\n");
        return -1;
    }

#endif

    return 0;
}

/*
 * calculate_sha256 - 用 OpenSSL EVP 接口计算文件的 SHA-256，输出小写十六进制字符串
 *
 * fd     : 已以只读方式打开的二进制文件描述符
 * output : 输出缓冲区，至少 65 字节（64 个十六进制字符 + 结尾 '\0'）
 *
 * 返回 0 表示成功，-1 表示内存分配、读取或摘要计算失败
 *
 * 采用 goto cleanup 的单出口写法，保证任何失败路径都能释放 EVP 上下文
 */
static int calculate_sha256(int fd, char output[65])
{
    /* 创建摘要上下文（堆上分配，需配合 EVP_MD_CTX_free 释放） */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();

    unsigned char buffer[8192];            /* 分块读取用的缓冲区 */
    unsigned char digest[EVP_MAX_MD_SIZE]; /* 存放原始摘要字节（SHA-256 为 32 字节） */
    unsigned int digest_len = 0;           /* 实际摘要长度 */

    const char *digits = "0123456789abcdef"; /* 字节 -> 十六进制字符的查表 */

    int result = -1; /* 默认失败，成功时改为 0 */

    if (ctx == NULL)
        return -1;

    /* 把文件偏移复位到开头（前面 pread 未移动偏移，这里显式保证），
       然后初始化 SHA-256 算法 */
    if (lseek(fd, 0, SEEK_SET) < 0 ||
        EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) != 1)
        goto cleanup;

    /* 循环分块读文件，边读边喂给摘要上下文，避免一次性把整个文件载入内存 */
    for (;;)
    {
        ssize_t n = read(fd, buffer, sizeof(buffer));

        if (n == 0)
            break; /* 读到文件末尾，结束循环 */

        if (n < 0)
        {
            /* 被信号打断时重试，其余错误直接失败 */
            if (errno == EINTR)
                continue;

            goto cleanup;
        }

        /* 把本块数据加入摘要计算 */
        if (EVP_DigestUpdate(ctx, buffer, (size_t)n) != 1)
            goto cleanup;
    }

    /* 结束计算并输出原始摘要，同时确认长度确实是 SHA-256 的 32 字节 */
    if (EVP_DigestFinal_ex(ctx, digest, &digest_len) != 1 ||
        digest_len != 32)
        goto cleanup;

    /* 每个摘要字节拆成两个十六进制字符写入输出缓冲区 */
    for (unsigned int i = 0; i < digest_len; i++)
    {
        output[i * 2]     = digits[digest[i] >> 4];   /* 高 4 位 */
        output[i * 2 + 1] = digits[digest[i] & 0x0f]; /* 低 4 位 */
    }

    /* 64 个字符后补结束符，形成可直接用 strcmp 比较的 C 字符串 */
    output[64] = '\0';
    result = 0;

cleanup:
    /* 无论成功失败都释放摘要上下文 */
    EVP_MD_CTX_free(ctx);
    return result;
}

/*
 * main - OTA 升级包校验入口
 *
 * argv[1] 必须是升级包目录，目录内至少包含:
 *   manifest.json : 升级包元数据
 *   smart_gateway : 目标应用二进制（文件名由 OTA_APP_NAME 固定）
 *
 * 校验顺序: 元数据解析 -> 元数据一致性 -> 二进制类型/大小 -> ELF 头 -> SHA-256
 * 任意一步失败都会跳到 cleanup 并返回 1
 */
int ota_verify_package(const char *package_directory)
{
    char manifest_path[PATH_MAX]; /* <包目录>/manifest.json 的完整路径 */
    char binary_path[PATH_MAX];   /* <包目录>/smart_gateway 的完整路径 */
    char actual_hash[65];         /* 实际计算出的 SHA-256 字符串 */

    /* manifest 中的各字段对象，均指向 root 内部，不需要单独释放 */
    json_object *root = NULL;         /* manifest 根对象 */
    json_object *format = NULL;       /* format_version: 包格式版本 */
    json_object *application = NULL;  /* application: 应用名 */
    json_object *version = NULL;      /* version: 应用版本号 */
    json_object *architecture = NULL; /* architecture: 目标架构 */
    json_object *binary = NULL;       /* binary: 二进制文件名 */
    json_object *size = NULL;         /* size: 二进制字节数 */
    json_object *sha256 = NULL;       /* sha256: 二进制摘要 */

    struct stat st; /* 保存 fstat 得到的文件属性 */
    int fd = -1;    /* 二进制文件描述符，-1 表示尚未打开 */
    int result = 1; /* 默认失败，全部校验通过后改为 0 */

    /* 步骤 2: 拼接 manifest.json 路径，并检查是否被截断 */
    int n = snprintf(
        manifest_path,
        sizeof(manifest_path),
        "%s/manifest.json",
        package_directory);

    if (n < 0 || (size_t)n >= sizeof(manifest_path))
    {
        fprintf(stderr, "FAIL: package path too long\n");
        return 1;
    }

    /* 步骤 3: 解析 JSON 文件到内存对象树 */
    root = json_object_from_file(manifest_path);

    /* 解析失败，或根节点不是对象（如数组/字符串），都视为非法清单 */
    if (root == NULL ||
        json_object_get_type(root) != json_type_object)
    {
        fprintf(stderr, "FAIL: invalid manifest JSON\n");
        goto cleanup;
    }

    /* 步骤 4: 一次性取出 7 个必需字段并校验类型，任一失败即退出 */
    if (get_field(root, "format_version", json_type_int, &format) ||
        get_field(root, "application", json_type_string, &application) ||
        get_field(root, "version", json_type_string, &version) ||
        get_field(root, "architecture", json_type_string, &architecture) ||
        get_field(root, "binary", json_type_string, &binary) ||
        get_field(root, "size", json_type_int, &size) ||
        get_field(root, "sha256", json_type_string, &sha256))
        goto cleanup;

    /* 取出期望值，后续与真实文件逐项比对 */
    const char *expected_hash = json_object_get_string(sha256);
    int64_t expected_size = json_object_get_int64(size);

    /* 步骤 5: 元数据一致性校验
     *   - 格式版本必须为 1
     *   - 应用名、二进制文件名、架构必须与目标一致
     *   - 版本号不能为空
     *   - 大小必须为正且不超过上限
     *   - 摘要字符串长度必须正好 64
     */
    if (json_object_get_int64(format) != 1 ||
        strcmp(json_object_get_string(application), OTA_APP_NAME) != 0 ||
        strcmp(json_object_get_string(architecture), OTA_ARCH) != 0 ||
        strcmp(json_object_get_string(binary), OTA_APP_NAME) != 0 ||
        json_object_get_string(version)[0] == '\0' ||
        expected_size <= 0 ||
        expected_size > OTA_MAX_SIZE ||
        strlen(expected_hash) != 64)
    {
        fprintf(stderr, "FAIL: manifest metadata mismatch\n");
        goto cleanup;
    }

    /* 步骤 6: 摘要格式校验，只接受小写十六进制字符（0-9a-f） */
    for (size_t i = 0; i < 64; i++)
    {
        char c = expected_hash[i];

        if (!((c >= '0' && c <= '9') ||
              (c >= 'a' && c <= 'f')))
        {
            fprintf(stderr, "FAIL: invalid SHA-256 format\n");
            goto cleanup;
        }
    }

    /* 步骤 7: 拼接二进制文件路径（包目录 + 固定应用名） */
    n = snprintf(
        binary_path,
        sizeof(binary_path),
        "%s/%s",
        package_directory,
        OTA_APP_NAME);

    if (n < 0 || (size_t)n >= sizeof(binary_path))
    {
        fprintf(stderr, "FAIL: binary path too long\n");
        goto cleanup;
    }

    /* 步骤 8: 以只读方式打开二进制
     *   O_RDONLY  : 只读，不修改升级包内容
     *   O_CLOEXEC : exec 时自动关闭，避免 fd 泄漏到子进程
     *   O_NOFOLLOW: 末段是符号链接时直接失败，防止通过软链指向任意文件
     */
    fd = open(binary_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);

    if (fd < 0)
    {
        perror("FAIL: open binary");
        goto cleanup;
    }

    /* 步骤 9: 校验文件类型与大小
     *   必须是普通文件（不是目录/设备/管道），且大小与 manifest 声明一致
     */
    if (fstat(fd, &st) != 0 ||
        !S_ISREG(st.st_mode) ||
        st.st_size != expected_size)
    {
        fprintf(stderr, "FAIL: binary size mismatch\n");
        goto cleanup;
    }

    /* 步骤 10: 校验 ELF 头，确认是目标平台可运行的 ARM 二进制 */
    if (check_elf(fd) != 0)
        goto cleanup;

    /* 步骤 11: 计算实际文件的 SHA-256 */
    if (calculate_sha256(fd, actual_hash) != 0)
    {
        fprintf(stderr, "FAIL: SHA-256 calculation error\n");
        goto cleanup;
    }

    /* 步骤 12: 摘要比对，防止内容被篡改或下载不完整 */
    if (strcmp(expected_hash, actual_hash) != 0)
    {
        fprintf(stderr, "FAIL: SHA-256 mismatch\n");
        goto cleanup;
    }

    /* 全部通过: 打印校验结果摘要，并把返回值改为 0 */
    printf("PASS: OTA package verification successful\n");
    printf("Application : %s\n", OTA_APP_NAME);
    printf("Version     : %s\n", json_object_get_string(version));
    printf("Size        : %lld bytes\n", (long long)expected_size);
    printf("SHA-256     : %s\n", actual_hash);

    result = 0;

cleanup:
    /* 统一清理: 关闭已打开的 fd，并释放 JSON 对象树（会连带释放所有字段对象） */
    if (fd >= 0)
        close(fd);

    if (root != NULL)
        json_object_put(root);

    return result;
}
