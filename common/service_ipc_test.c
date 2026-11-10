#define _GNU_SOURCE 1

#include "service_ipc.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

static int checks = 0;
static int failures = 0;

static void check(const char *name, int ok)
{
    checks++;

    if (!ok) {
        failures++;
        printf("FAIL: %s\n", name);
    } else {
        printf("PASS: %s\n", name);
    }
}

static void test_request_roundtrip(void)
{
    struct {
        service_ipc_request_t request;
        const char *text;
    } cases[] = {
        { SERVICE_IPC_STATUS, "STATUS\n" },
        { SERVICE_IPC_START,  "START\n"  },
        { SERVICE_IPC_STOP,   "STOP\n"   },
        { SERVICE_IPC_RELOAD, "RELOAD\n" },
    };

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        char buffer[64];

        int n = service_ipc_encode_request(
            cases[i].request, buffer, sizeof(buffer));

        check("请求编码成功", n > 0);
        check("请求编码内容正确",
              n > 0 && strcmp(buffer, cases[i].text) == 0);

        service_ipc_request_t parsed = SERVICE_IPC_UNKNOWN;

        check("请求解码成功",
              service_ipc_decode_request(buffer, &parsed) == 0);
        check("请求解码一致", parsed == cases[i].request);
    }
}

static void test_request_whitespace(void)
{
    /*
     * 允许尾部空白与 \r\n。
     *
     * 现实中客户端可能是 telnet 或 Windows 风格换行，
     * 严格只认 "STATUS\n" 会让手工调试变得不可能。
     */
    service_ipc_request_t parsed = SERVICE_IPC_UNKNOWN;

    check("接受 \\r\\n", service_ipc_decode_request(
              "STATUS\r\n", &parsed) == 0 &&
          parsed == SERVICE_IPC_STATUS);

    check("接受前导空格", service_ipc_decode_request(
              "  STOP  ", &parsed) == 0 &&
          parsed == SERVICE_IPC_STOP);

    check("接受无换行", service_ipc_decode_request(
              "RELOAD", &parsed) == 0 &&
          parsed == SERVICE_IPC_RELOAD);
}

static void test_request_invalid(void)
{
    service_ipc_request_t parsed = SERVICE_IPC_UNKNOWN;

    check("拒绝空串",
          service_ipc_decode_request("", &parsed) != 0 &&
          parsed == SERVICE_IPC_UNKNOWN);

    check("拒绝未知命令",
          service_ipc_decode_request("DELETE ALL\n", &parsed) != 0 &&
          parsed == SERVICE_IPC_UNKNOWN);

    check("拒绝前缀匹配",
          service_ipc_decode_request("STAR\n", &parsed) != 0);

    check("拒绝超长输入",
          service_ipc_decode_request(
              "STATUS STATUS STATUS STATUS STATUS STATUS STATUS "
              "STATUS STATUS STATUS STATUS STATUS STATUS STATUS "
              "STATUS STATUS STATUS STATUS STATUS STATUS STATUS "
              "STATUS STATUS STATUS STATUS STATUS STATUS STATUS "
              "STATUS STATUS STATUS STATUS STATUS STATUS STATUS",
              &parsed) != 0);
}

static void test_reply_roundtrip(void)
{
    service_ipc_reply_t reply = {
        .result = SERVICE_IPC_OK,
        .pid = 1234,
        .starttime = 987654321ULL,
        .detail = "running",
    };

    char buffer[256];

    int n = service_ipc_encode_reply(&reply, buffer, sizeof(buffer));

    check("响应编码成功", n > 0);

    service_ipc_reply_t parsed;

    check("响应解码成功",
          service_ipc_decode_reply(buffer, &parsed) == 0);
    check("响应 result 一致", parsed.result == SERVICE_IPC_OK);
    check("响应 pid 一致", parsed.pid == 1234);
    check("响应 starttime 一致", parsed.starttime == 987654321ULL);
    check("响应 detail 一致", strcmp(parsed.detail, "running") == 0);
}

static void test_reply_multiline_detail(void)
{
    /*
     * detail 里的换行必须被压成空格。
     *
     * 否则客户端按行读取时会把后半截当成下一条响应的开头，
     * 协议就散了。这条不是理论问题——errno 字符串里
     * 通常不含换行，但 strerror 在某些 locale 下可能含。
     */
    service_ipc_reply_t reply = {
        .result = SERVICE_IPC_FAILED,
        .pid = 0,
        .starttime = 0,
        .detail = "line1\nline2\rline3",
    };

    char buffer[256];

    check("编码含换行的 detail 成功",
          service_ipc_encode_reply(&reply, buffer, sizeof(buffer)) > 0);

    check("换行被压缩为空格",
          strchr(buffer, '\n') != NULL &&
          strchr(buffer, '\n') == buffer + strlen(buffer) - 1);

    /* 只应有一个换行（结尾那个） */
    int count = 0;

    for (const char *p = buffer; *p != '\0'; p++) {
        if (*p == '\n')
            count++;
    }

    check("整体只有结尾一个换行", count == 1);
}

static void test_reply_empty_detail(void)
{
    service_ipc_reply_t reply = {
        .result = SERVICE_IPC_NOT_RUNNING,
        .pid = 0,
        .starttime = 0,
        .detail = "",
    };

    char buffer[128];

    check("空 detail 编码成功",
          service_ipc_encode_reply(&reply, buffer, sizeof(buffer)) > 0);

    service_ipc_reply_t parsed;

    check("空 detail 解码成功",
          service_ipc_decode_reply(buffer, &parsed) == 0);
    check("空 detail 结果为 NOT_RUNNING",
          parsed.result == SERVICE_IPC_NOT_RUNNING);
    check("空 detail 为空串", parsed.detail[0] == '\0');
}

static void test_reply_invalid(void)
{
    service_ipc_reply_t parsed;

    check("拒绝空响应",
          service_ipc_decode_reply("", &parsed) != 0);

    check("拒绝未知 result",
          service_ipc_decode_reply("MAYBE 1 2 x\n", &parsed) != 0);

    check("拒绝缺字段",
          service_ipc_decode_reply("OK\n", &parsed) != 0);

    check("拒绝非数字 pid",
          service_ipc_decode_reply("OK abc 2 x\n", &parsed) != 0);

    check("拒绝负 pid",
          service_ipc_decode_reply("OK -5 2 x\n", &parsed) != 0);

    /*
     * 溢出必须拒绝。
     *
     * 这是 strtoull 的经典陷阱：溢出时返回 ULLONG_MAX
     * 且 errno == ERANGE，若只看返回值会当成合法值接受。
     * OTA 的 pidfile 解析踩过一次同样的坑。
     */
    check("拒绝溢出的 starttime",
          service_ipc_decode_reply(
              "OK 1 99999999999999999999999999 x\n", &parsed) != 0);

    check("拒绝非数字 starttime",
          service_ipc_decode_reply("OK 1 abc x\n", &parsed) != 0);
}

static void test_path(void)
{
    char path[128];

    check("路径生成成功",
          service_ipc_path("/opt/sg/run", path, sizeof(path)) == 0);

    check("路径内容正确",
          strcmp(path, "/opt/sg/run/supervisor.sock") == 0);

    /* 截断必须报错，不能悄悄截断到另一个路径 */
    char small[10];

    check("路径截断时报错",
          service_ipc_path("/opt/sg/run", small, sizeof(small)) != 0);

    check("空目录名报错",
          service_ipc_path("", path, sizeof(path)) != 0);
}

/*
 * socket 权限必须是 0600。
 *
 * 能停止服务的人等于能中断业务，不该给普通用户。
 * 这条在板端尤其重要：BusyBox 环境下 telnet 是开着的。
 */
static void test_socket_permissions(void)
{
    char dir[] = "/tmp/sipc_test_XXXXXX";
    char *base = mkdtemp(dir);

    if (base == NULL) {
        check("创建临时目录", 0);
        return;
    }

    char path[256];
    int fd = service_ipc_listen(base, path, sizeof(path));

    check("创建监听 socket", fd >= 0);

    if (fd >= 0) {
        struct stat st;

        check("socket 文件存在",
              stat(path, &st) == 0 && S_ISSOCK(st.st_mode));

        check("socket 权限为 0600",
              (st.st_mode & 0777) == 0600);

        /*
         * 已有实例在监听时必须拒绝第二次绑定。
         *
         * 若不检查，两个 supervisor 会都认为自己是唯一的，
         * 于是双方各启动一个 Gateway。
         */
        char path2[256];
        int fd2 = service_ipc_listen(base, path2, sizeof(path2));

        check("重复绑定被拒绝", fd2 < 0 && errno == EADDRINUSE);

        if (fd2 >= 0)
            close(fd2);

        close(fd);
        unlink(path);
    }

    rmdir(base);
}

/*
 * 陈旧 socket 文件应能被自动清理。
 *
 * 场景：supervisor 被 kill -9，socket 文件残留但无人监听。
 * 若不清理，下次启动会永远失败，必须人工 rm。
 */
static void test_stale_socket_recovery(void)
{
    char dir[] = "/tmp/sipc_stale_XXXXXX";
    char *base = mkdtemp(dir);

    if (base == NULL) {
        check("创建临时目录", 0);
        return;
    }

    char path[256];

    snprintf(path, sizeof(path), "%s/%s",
             base, SERVICE_IPC_SOCKET_NAME);

    /* 手工创建一个"看起来像"陈旧文件的 socket */
    int s = socket(AF_UNIX, SOCK_STREAM, 0);

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, path, strlen(path) + 1);

    check("创建陈旧 socket 文件",
          s >= 0 && bind(s, (struct sockaddr *)&addr, sizeof(addr)) == 0);

    close(s);
    /* 注意：此时没有监听者，文件仍然在 */

    char got[256];
    int fd = service_ipc_listen(base, got, sizeof(got));

    check("陈旧 socket 被自动清理并重建", fd >= 0);

    if (fd >= 0) {
        close(fd);
        unlink(path);
    }

    rmdir(base);
}

int main(void)
{
    printf("== service_ipc 协议测试 ==\n\n");

    test_request_roundtrip();
    printf("\n");
    test_request_whitespace();
    printf("\n");
    test_request_invalid();
    printf("\n");
    test_reply_roundtrip();
    printf("\n");
    test_reply_multiline_detail();
    printf("\n");
    test_reply_empty_detail();
    printf("\n");
    test_reply_invalid();
    printf("\n");
    test_path();
    printf("\n");
    test_socket_permissions();
    printf("\n");
    test_stale_socket_recovery();

    printf("\n检查项: %d, 失败: %d\n", checks, failures);
    printf("结果: %s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
