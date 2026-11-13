#define _POSIX_C_SOURCE 200809L
#include "fw_env_backend.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static void set_err(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen > 0) {
        snprintf(errbuf, errlen, "%s", msg ? msg : "unknown error");
    }
}

void fw_env_backend_cfg_from_env(fw_env_backend_cfg_t *cfg)
{
    const char *v;

    if (!cfg)
        return;

    v = getenv("SGW_FW_PRINTENV");
    cfg->fw_printenv_bin = (v && v[0]) ? v : "fw_printenv";

    v = getenv("SGW_FW_SETENV");
    cfg->fw_setenv_bin = (v && v[0]) ? v : "fw_setenv";

    v = getenv("SGW_FW_ENV_CONFIG");
    cfg->config_path = (v && v[0]) ? v : "/etc/fw_env.config";
}

static int is_allowed_ab_write_var(const char *key)
{
    if (!key || !key[0])
        return 0;
    if (ab_is_swupdate_reserved_var(key))
        return 0;
    if (strncmp(key, "sgw_", 4) == 0)
        return 1;
    if (strcmp(key, "bootlimit") == 0 ||
        strcmp(key, "bootcount") == 0 ||
        strcmp(key, "upgrade_available") == 0)
        return 1;
    return 0;
}

int fw_env_backend_check_script_line(const char *line,
                                     char *errbuf,
                                     size_t errlen)
{
    char key[128];
    size_t i = 0;
    const char *p = line;

    if (!p) {
        set_err(errbuf, errlen, "null script line");
        return -1;
    }

    while (*p && isspace((unsigned char)*p))
        p++;

    if (*p == '\0' || *p == '#')
        return 0;

    while (*p && !isspace((unsigned char)*p) && i + 1 < sizeof(key)) {
        key[i++] = *p++;
    }
    key[i] = '\0';

    if (ab_is_swupdate_reserved_var(key)) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "refusing to write SWUpdate-reserved variable '%s'", key);
        return -1;
    }
    if (!is_allowed_ab_write_var(key)) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "variable '%s' is outside A/B allowlist", key);
        return -1;
    }
    return 0;
}

static int parse_ulong_strict(const char *val, unsigned long *out)
{
    char *end = NULL;
    unsigned long parsed;

    if (!val || !val[0] || !out)
        return -1;
    errno = 0;
    parsed = strtoul(val, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -1;
    *out = parsed;
    return 0;
}

static void apply_kv(ab_env_t *env, const char *key, const char *val)
{
    unsigned long ul = 0;

    if (strcmp(key, "sgw_active_slot") == 0) {
        if (ab_slot_parse(val, &env->active_slot) != 0)
            env->active_slot = AB_SLOT_NONE;
    } else if (strcmp(key, "sgw_last_good_slot") == 0) {
        if (ab_slot_parse(val, &env->last_good_slot) != 0)
            env->last_good_slot = AB_SLOT_NONE;
    } else if (strcmp(key, "sgw_pending_slot") == 0) {
        if (ab_slot_parse(val, &env->pending_slot) != 0)
            env->pending_slot = AB_SLOT_NONE;
    } else if (strcmp(key, "sgw_ab_state") == 0) {
        ab_state_parse(val, &env->ab_state);
    } else if (strcmp(key, "bootlimit") == 0) {
        if (parse_ulong_strict(val, &ul) == 0)
            env->bootlimit = ul;
    } else if (strcmp(key, "bootcount") == 0) {
        if (parse_ulong_strict(val, &ul) == 0)
            env->bootcount = ul;
    } else if (strcmp(key, "upgrade_available") == 0) {
        if (parse_ulong_strict(val, &ul) == 0)
            env->upgrade_available = (int)ul;
    } else if (strcmp(key, "sgw_rollback_count") == 0) {
        if (parse_ulong_strict(val, &ul) == 0)
            env->rollback_count = ul;
    } else if (strcmp(key, "sgw_last_error") == 0) {
        snprintf(env->last_error, sizeof(env->last_error), "%s", val);
    } else if (strcmp(key, "update") == 0) {
        snprintf(env->swu_update, sizeof(env->swu_update), "%s", val);
    } else if (strcmp(key, "recovery_status") == 0) {
        snprintf(env->swu_recovery_status, sizeof(env->swu_recovery_status),
                 "%s", val);
    }
}

static int read_all_fd(int fd, char *buf, size_t cap, size_t *out_len)
{
    size_t used = 0;

    while (used + 1 < cap) {
        ssize_t n = read(fd, buf + used, cap - 1 - used);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            break;
        used += (size_t)n;
    }
    buf[used] = '\0';
    if (out_len)
        *out_len = used;
    return 0;
}

static int write_all_fd(int fd, const char *buf, size_t len)
{
    size_t sent = 0;

    while (sent < len) {
        ssize_t n = write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        sent += (size_t)n;
    }
    return 0;
}

static int run_child_capture(const char *bin,
                             char *const argv[],
                             const char *stdin_data,
                             char *stdout_buf,
                             size_t stdout_cap,
                             char *stderr_buf,
                             size_t stderr_cap,
                             int *exit_code)
{
    int in_pipe[2] = {-1, -1};
    int out_pipe[2] = {-1, -1};
    int err_pipe[2] = {-1, -1};
    pid_t pid;
    int status = 0;

    if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
        goto fail_pipes;
    if (stdin_data && pipe(in_pipe) != 0)
        goto fail_pipes;

    pid = fork();
    if (pid < 0)
        goto fail_pipes;

    if (pid == 0) {
        if (stdin_data) {
            dup2(in_pipe[0], STDIN_FILENO);
            close(in_pipe[0]);
            close(in_pipe[1]);
        } else {
            int devnull = open("/dev/null", O_RDONLY);
            if (devnull >= 0) {
                dup2(devnull, STDIN_FILENO);
                close(devnull);
            }
        }
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(err_pipe[1], STDERR_FILENO);
        close(out_pipe[0]);
        close(out_pipe[1]);
        close(err_pipe[0]);
        close(err_pipe[1]);
        execvp(bin, argv);
        _exit(127);
    }

    close(out_pipe[1]);
    close(err_pipe[1]);
    if (stdin_data) {
        close(in_pipe[0]);
        if (write_all_fd(in_pipe[1], stdin_data, strlen(stdin_data)) != 0) {
            close(in_pipe[1]);
            waitpid(pid, &status, 0);
            close(out_pipe[0]);
            close(err_pipe[0]);
            return -1;
        }
        close(in_pipe[1]);
    }

    if (stdout_buf && stdout_cap > 0)
        read_all_fd(out_pipe[0], stdout_buf, stdout_cap, NULL);
    close(out_pipe[0]);

    if (stderr_buf && stderr_cap > 0)
        read_all_fd(err_pipe[0], stderr_buf, stderr_cap, NULL);
    close(err_pipe[0]);

    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR)
            return -1;
    }

    if (WIFEXITED(status)) {
        *exit_code = WEXITSTATUS(status);
        return 0;
    }
    *exit_code = -1;
    return -1;

fail_pipes:
    for (int i = 0; i < 2; i++) {
        if (in_pipe[i] >= 0) close(in_pipe[i]);
        if (out_pipe[i] >= 0) close(out_pipe[i]);
        if (err_pipe[i] >= 0) close(err_pipe[i]);
    }
    return -1;
}

int fw_env_backend_load(const fw_env_backend_cfg_t *cfg,
                        ab_env_t *env,
                        char *errbuf,
                        size_t errlen)
{
    fw_env_backend_cfg_t effective;
    char stdout_buf[16384];
    char stderr_buf[1024];
    char *argv[5];
    int argc = 0;
    int exit_code = -1;
    char *line;
    char *saveptr = NULL;

    if (!env) {
        set_err(errbuf, errlen, "null env");
        return -1;
    }

    ab_env_init_default(env);

    if (cfg) {
        effective = *cfg;
    } else {
        fw_env_backend_cfg_from_env(&effective);
    }
    if (!effective.fw_printenv_bin || !effective.fw_printenv_bin[0])
        effective.fw_printenv_bin = "fw_printenv";

    argv[argc++] = (char *)effective.fw_printenv_bin;
    if (effective.config_path && effective.config_path[0]) {
        argv[argc++] = "-c";
        argv[argc++] = (char *)effective.config_path;
    }
    argv[argc] = NULL;

    stdout_buf[0] = '\0';
    stderr_buf[0] = '\0';
    if (run_child_capture(effective.fw_printenv_bin, argv, NULL,
                          stdout_buf, sizeof(stdout_buf),
                          stderr_buf, sizeof(stderr_buf), &exit_code) != 0 ||
        exit_code != 0) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "fw_printenv failed (exit=%d): %.200s",
                 exit_code, stderr_buf[0] ? stderr_buf : "exec error");
        return -1;
    }

    line = strtok_r(stdout_buf, "\n", &saveptr);
    while (line) {
        size_t n = strlen(line);
        char *eq;

        if (n > 0 && line[n - 1] == '\r')
            line[n - 1] = '\0';
        eq = strchr(line, '=');
        if (eq) {
            *eq = '\0';
            apply_kv(env, line, eq + 1);
        }
        line = strtok_r(NULL, "\n", &saveptr);
    }

    return 0;
}

static void sanitize_token(const char *src, char *dst, size_t cap)
{
    size_t i = 0;

    if (!dst || cap == 0)
        return;
    if (!src || !src[0]) {
        snprintf(dst, cap, "%s", "none");
        return;
    }
    while (src[i] && i + 1 < cap) {
        char c = src[i];
        if (c == '\n' || c == '\r')
            c = '_';
        dst[i] = c;
        i++;
    }
    dst[i] = '\0';
}

int fw_env_backend_save(const fw_env_backend_cfg_t *cfg,
                        const ab_env_t *env,
                        char *errbuf,
                        size_t errlen)
{
    fw_env_backend_cfg_t effective;
    char safe_err[64];
    char script[1024];
    char script_copy[1024];
    char stdout_buf[1024];
    char stderr_buf[1024];
    char *argv[6];
    int argc = 0;
    int exit_code = -1;
    int written;
    char *line;
    char *saveptr = NULL;

    if (!env) {
        set_err(errbuf, errlen, "null env");
        return -1;
    }
    if (ab_env_validate(env, errbuf, errlen) != 0)
        return -1;

    if (cfg) {
        effective = *cfg;
    } else {
        fw_env_backend_cfg_from_env(&effective);
    }
    if (!effective.fw_setenv_bin || !effective.fw_setenv_bin[0])
        effective.fw_setenv_bin = "fw_setenv";

    sanitize_token(env->last_error, safe_err, sizeof(safe_err));

    written = snprintf(script, sizeof(script),
                       "sgw_active_slot %s\n"
                       "sgw_last_good_slot %s\n"
                       "sgw_pending_slot %s\n"
                       "sgw_ab_state %s\n"
                       "bootlimit %lu\n"
                       "bootcount %lu\n"
                       "upgrade_available %d\n"
                       "sgw_rollback_count %lu\n"
                       "sgw_last_error %s\n",
                       ab_slot_str(env->active_slot),
                       ab_slot_str(env->last_good_slot),
                       ab_slot_str(env->pending_slot),
                       ab_state_str(env->ab_state),
                       env->bootlimit,
                       env->bootcount,
                       env->upgrade_available,
                       env->rollback_count,
                       safe_err);
    if (written < 0 || (size_t)written >= sizeof(script)) {
        set_err(errbuf, errlen, "script buffer overflow");
        return -1;
    }

    snprintf(script_copy, sizeof(script_copy), "%s", script);
    line = strtok_r(script_copy, "\n", &saveptr);
    while (line) {
        if (fw_env_backend_check_script_line(line, errbuf, errlen) != 0)
            return -1;
        line = strtok_r(NULL, "\n", &saveptr);
    }

    argv[argc++] = (char *)effective.fw_setenv_bin;
    if (effective.config_path && effective.config_path[0]) {
        argv[argc++] = "-c";
        argv[argc++] = (char *)effective.config_path;
    }
    argv[argc++] = "-s";
    argv[argc++] = "-";
    argv[argc] = NULL;

    stdout_buf[0] = '\0';
    stderr_buf[0] = '\0';
    if (run_child_capture(effective.fw_setenv_bin, argv, script,
                          stdout_buf, sizeof(stdout_buf),
                          stderr_buf, sizeof(stderr_buf), &exit_code) != 0 ||
        exit_code != 0) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "fw_setenv failed (exit=%d): %.200s",
                 exit_code, stderr_buf[0] ? stderr_buf : "exec error");
        return -1;
    }

    return 0;
}
