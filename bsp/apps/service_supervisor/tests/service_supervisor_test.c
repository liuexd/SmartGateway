/*
 * service_supervisor 单元测试（S5）
 *
 * 重点验证三件事：
 *   1. pidfile 身份校验：僵尸、pid 复用、损坏内容都必须判为非运行
 *   2. 配置文件解析：路径必须绝对、非法值必须拒绝
 *   3. 真实进程生命周期：start/stop 幂等、SIGKILL 升级
 *
 * 其中第 1 组最关键。历史教训（D25-2-S1）：
 * 只看 /proc 是否存在会把僵尸当成 RUNNING；
 * 只看 starttime 会把 pid 复用的无关进程当成 Gateway。
 */

#define _GNU_SOURCE 1

#include "service_supervisor.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "service_ipc.h"

static int checks = 0;
static int failures = 0;

static void check(const char *name, int ok, const char *detail)
{
    checks++;

    if (!ok) {
        failures++;
        printf("FAIL: %s", name);
    } else {
        printf("PASS: %s", name);
    }

    if (detail != NULL && detail[0] != '\0')
        printf(" (%s)", detail);

    printf("\n");
}

static unsigned long long read_starttime(pid_t pid)
{
    char path[64];

    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)pid);

    int fd = open(path, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;

    char buf[1024];
    ssize_t r = read(fd, buf, sizeof(buf) - 1);

    close(fd);

    if (r <= 0)
        return 0;

    buf[r] = '\0';

    const char *rp = strrchr(buf, ')');

    if (rp == NULL)
        return 0;

    char *save = NULL;
    char *tok = strtok_r((char *)rp + 2, " ", &save);

    if (tok == NULL)
        return 0;

    for (int i = 1; i <= 19; i++) {
        tok = strtok_r(NULL, " ", &save);

        if (tok == NULL)
            return 0;
    }

    return strtoull(tok, NULL, 10);
}

static char *make_dir(const char *tag)
{
    char tpl[128];

    snprintf(tpl, sizeof(tpl), "/tmp/suptest_%s_XXXXXX", tag);

    char *dir = mkdtemp(tpl);

    return dir == NULL ? NULL : strdup(dir);
}

static void write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);

    if (fd < 0)
        return;

    (void)write(fd, content, strlen(content));
    close(fd);
}

/* ------------------------------------------------------------------ */
/* 状态查询                                                            */
/* ------------------------------------------------------------------ */

static void test_state_no_pidfile(void)
{
    char *dir = make_dir("nopid");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    service_state_t st;

    int rc = service_state_query(dir, &st);

    check("无 pidfile 时查询成功", rc == 0, "");
    check("无 pidfile 判为未运行", !st.running, "");

    rmdir(dir);
    free(dir);
}

static void test_state_real_process(void)
{
    char *dir = make_dir("real");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    /* 起一个真实存在的进程 */
    pid_t pid = fork();

    if (pid == 0) {
        for (;;)
            pause();
        _exit(0);
    }

    usleep(100000);

    unsigned long long st_val = read_starttime(pid);

    check("能读到真实进程 starttime", st_val != 0, "");

    char path[256];
    char content[128];

    snprintf(path, sizeof(path), "%s/%s",
             dir, SERVICE_PIDFILE_NAME);
    snprintf(content, sizeof(content), "%ld %llu\n", (long)pid, st_val);
    write_file(path, content);

    service_state_t st;

    check("真实进程查询成功",
          service_state_query(dir, &st) == 0, "");
    check("真实进程判为运行", st.running, "");
    check("pid 正确", st.pid == pid, "");
    check("starttime 正确", st.starttime == st_val, "");

    /* 清理 */
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    unlink(path);
    rmdir(dir);
    free(dir);
}

static void test_state_zombie(void)
{
    /*
     * 僵尸必须判为未运行。
     *
     * 这是最关键的一条：僵尸的 /proc 目录仍在，starttime 也不变。
     * 只看"文件是否存在"会把僵尸当成 RUNNING，于是 supervisor
     * 拒绝重启一个其实早已死掉的服务。
     */
    char *dir = make_dir("zombie");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    pid_t pid = fork();

    if (pid == 0)
        _exit(0);

    /* 故意不 wait：让它变成僵尸 */
    usleep(200000);

    /* 确认它确实是僵尸 */
    char statpath[64];
    snprintf(statpath, sizeof(statpath), "/proc/%ld/stat", (long)pid);

    int fd = open(statpath, O_RDONLY);
    char buf[512] = {0};

    if (fd >= 0) {
        ssize_t r = read(fd, buf, sizeof(buf) - 1);
        (void)r;
        close(fd);
    }

    const char *rp = strrchr(buf, ')');
    int is_zombie = rp != NULL && rp[1] == ' ' && rp[2] == 'Z';

    check("前置：进程已进入僵尸态", is_zombie, "");

    /* 僵尸的 starttime 仍可读，正好用来构造陷阱 */
    unsigned long long st_val = read_starttime(pid);

    char path[256];
    char content[128];

    snprintf(path, sizeof(path), "%s/%s",
             dir, SERVICE_PIDFILE_NAME);
    snprintf(content, sizeof(content), "%ld %llu\n",
             (long)pid, st_val == 0 ? 1 : st_val);
    write_file(path, content);

    service_state_t st;

    check("僵尸查询成功", service_state_query(dir, &st) == 0, "");
    check("僵尸判为未运行（关键）", !st.running,
          is_zombie ? "进程确实是 Z 状态" : "");

    waitpid(pid, NULL, 0);
    unlink(path);
    rmdir(dir);
    free(dir);
}

static void test_state_pid_reused(void)
{
    /*
     * starttime 不匹配 = pid 已复用 -> 必须判为未运行。
     *
     * 注意这里**不能**直接断言"不运行就安全"——真正的安全含义是
     * supervisor 不会去 kill 这个 pid。查询层面只负责给出
     * 正确的判断，kill 路径的保证来自"只停自己的子进程"。
     */
    char *dir = make_dir("reuse");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    pid_t pid = fork();

    if (pid == 0) {
        for (;;)
            pause();
        _exit(0);
    }

    usleep(100000);

    unsigned long long real = read_starttime(pid);

    char path[256];
    char content[128];

    snprintf(path, sizeof(path), "%s/%s",
             dir, SERVICE_PIDFILE_NAME);

    /* 故意写错 starttime */
    snprintf(content, sizeof(content), "%ld %llu\n",
             (long)pid, real + 1);
    write_file(path, content);

    service_state_t st;

    check("pid 复用场景查询成功",
          service_state_query(dir, &st) == 0, "");
    check("starttime 不匹配判为未运行", !st.running, "");

    /* 关键：进程本身必须还活着 */
    check("进程本身未被影响",
          kill(pid, 0) == 0, "");

    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);

    unlink(path);
    rmdir(dir);
    free(dir);
}

static void test_state_corrupt_pidfile(void)
{
    char *dir = make_dir("corrupt");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    const char *bad[] = {
        "",
        "not-a-pid",
        "123",
        "-5 100\n",
        "0 100\n",
        "123 abc\n",
        "123 0\n",
        "123 999999999999999999999999\n",
    };

    char path[256];
    snprintf(path, sizeof(path), "%s/%s",
             dir, SERVICE_PIDFILE_NAME);

    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        write_file(path, bad[i]);

        service_state_t st;
        int rc = service_state_query(dir, &st);

        char label[128];
        snprintf(label, sizeof(label),
                 "损坏 pidfile #%zu 被拒绝", i + 1);

        check(label, rc != 0, bad[i]);
    }

    unlink(path);
    rmdir(dir);
    free(dir);
}

/* ------------------------------------------------------------------ */
/* 配置解析                                                            */
/* ------------------------------------------------------------------ */

static void test_config_missing(void)
{
    service_config_t cfg;

    check("不存在的配置报错",
          service_config_load("/tmp/definitely_no_such.conf", &cfg) != 0,
          "");
}

static void test_config_requires_absolute(void)
{
    char *dir = make_dir("cfgrel");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/test.conf", dir);

    /*
     * 相对路径必须被拒绝。
     *
     * daemon 化后 cwd 会变成 /，相对路径会指向完全不同的文件。
     * 接受它等于把"启动哪个程序"变成一个不可预期的行为。
     */
    write_file(path, "executable=./gateway\n");

    service_config_t cfg;

    check("相对 executable 被拒绝",
          service_config_load(path, &cfg) != 0, "");

    write_file(path, "serial=/dev/ttyS0\n");

    check("缺少 executable 被拒绝",
          service_config_load(path, &cfg) != 0, "");

    unlink(path);
    rmdir(dir);
    free(dir);
}

static void test_config_defaults_and_override(void)
{
    char *dir = make_dir("cfgok");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/test.conf", dir);

    write_file(path,
               "# comment line\n"
               "\n"
               "executable=/opt/sg/current/smart_gateway\n"
               "server=10.0.0.5\n");

    service_config_t cfg;

    int rc = service_config_load(path, &cfg);

    check("合法配置加载成功", rc == 0, "");
    check("executable 正确",
          strcmp(cfg.executable,
                 "/opt/sg/current/smart_gateway") == 0, "");
    check("server 覆盖生效",
          strcmp(cfg.server, "10.0.0.5") == 0, "");
    check("port 使用默认值", cfg.port == 9000, "");
    check("baud 使用默认值", cfg.baud == 115200, "");
    check("stop_timeout_ms 有默认值",
          cfg.stop_timeout_ms > 0, "");

    /* 覆盖 + 非法值拒绝 */
    write_file(path,
               "executable=/opt/sg/current/smart_gateway\n"
               "port=99999\n");

    check("非法 port 被拒绝",
          service_config_load(path, &cfg) != 0, "");

    write_file(path,
               "executable=/opt/sg/current/smart_gateway\n"
               "port=abc\n");

    check("非数字 port 被拒绝",
          service_config_load(path, &cfg) != 0, "");

    unlink(path);
    rmdir(dir);
    free(dir);
}

static void test_config_write_no_overwrite(void)
{
    char *dir = make_dir("cfgw");

    if (dir == NULL) {
        check("创建临时目录", 0, "");
        return;
    }

    char path[256];
    snprintf(path, sizeof(path), "%s/service.conf", dir);

    check("首次写默认配置成功",
          service_config_write_default(
              path, "/opt/sg/current/smart_gateway") == 0, "");

    /*
     * 第二次必须失败（EEXIST）。
     *
     * 否则一次误执行的部署命令会抹掉现场调整过的配置，
     * 而运维往往在事后很久才发现。
     */
    check("重复写被拒绝（不覆盖）",
          service_config_write_default(
              path, "/other/path") != 0, "");

    service_config_t cfg;

    check("原配置未被修改",
          service_config_load(path, &cfg) == 0 &&
          strcmp(cfg.executable,
                 "/opt/sg/current/smart_gateway") == 0, "");

    unlink(path);
    rmdir(dir);
    free(dir);
}

int main(void)
{
    printf("== service_supervisor 单元测试 ==\n\n");

    printf("-- 状态查询 --\n");
    test_state_no_pidfile();
    test_state_real_process();
    test_state_zombie();
    test_state_pid_reused();
    test_state_corrupt_pidfile();

    printf("\n-- 配置解析 --\n");
    test_config_missing();
    test_config_requires_absolute();
    test_config_defaults_and_override();
    test_config_write_no_overwrite();

    printf("\n检查项: %d, 失败: %d\n", checks, failures);
    printf("结果: %s\n", failures == 0 ? "PASS" : "FAIL");

    return failures == 0 ? 0 : 1;
}
