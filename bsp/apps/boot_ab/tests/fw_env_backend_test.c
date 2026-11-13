#define _POSIX_C_SOURCE 200809L
#include "fw_env_backend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(cond) do {     if (!(cond)) {         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         g_fail++;     } else {         g_pass++;     } } while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_STREQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

static void write_exec(const char *path, const char *body)
{
    FILE *fp = fopen(path, "w");
    ASSERT_TRUE(fp != NULL);
    if (!fp)
        return;
    fputs(body, fp);
    fclose(fp);
    ASSERT_EQ(chmod(path, 0755), 0);
}

int main(void)
{
    char err[256] = {0};
    char tmpdir[] = "/tmp/sgw_fw_backend_XXXXXX";
    char store_path[256];
    char print_bin[256];
    char set_bin[256];
    char cfg_path[256];
    char script[1024];
    fw_env_backend_cfg_t cfg;
    ab_env_t env;

    ASSERT_EQ(fw_env_backend_check_script_line("sgw_active_slot 1", err, sizeof(err)), 0);
    ASSERT_EQ(fw_env_backend_check_script_line("bootcount 2", err, sizeof(err)), 0);
    ASSERT_EQ(fw_env_backend_check_script_line("upgrade_available 1", err, sizeof(err)), 0);
    ASSERT_TRUE(fw_env_backend_check_script_line("update yes", err, sizeof(err)) != 0);
    ASSERT_TRUE(fw_env_backend_check_script_line("recovery_status in_progress", err, sizeof(err)) != 0);
    ASSERT_TRUE(fw_env_backend_check_script_line("ethaddr 00:11:22:33:44:55", err, sizeof(err)) != 0);

    ASSERT_TRUE(mkdtemp(tmpdir) != NULL);
    snprintf(store_path, sizeof(store_path), "%s/env.txt", tmpdir);
    snprintf(print_bin, sizeof(print_bin), "%s/mock_fw_printenv.sh", tmpdir);
    snprintf(set_bin, sizeof(set_bin), "%s/mock_fw_setenv.sh", tmpdir);
    snprintf(cfg_path, sizeof(cfg_path), "%s/fw_env.config", tmpdir);

    {
        FILE *fp = fopen(store_path, "w");
        ASSERT_TRUE(fp != NULL);
        fputs("update=yes\n"
              "recovery_status=idle_sentinel\n"
              "sgw_active_slot=2\n"
              "sgw_last_good_slot=2\n"
              "sgw_pending_slot=none\n"
              "sgw_ab_state=confirmed\n"
              "bootlimit=3\n"
              "bootcount=0\n"
              "upgrade_available=0\n"
              "sgw_rollback_count=0\n"
              "sgw_last_error=none\n", fp);
        fclose(fp);
    }

    snprintf(script, sizeof(script),
             "#!/bin/sh\n"
             "cat '%s'\n",
             store_path);
    write_exec(print_bin, script);

    snprintf(script, sizeof(script),
             "#!/usr/bin/env python3\n"
             "import sys\n"
             "path = '%s'\n"
             "kv = {}\n"
             "with open(path) as f:\n"
             "    for line in f:\n"
             "        if '=' in line:\n"
             "            k, v = line.rstrip('\\n').split('=', 1)\n"
             "            kv[k] = v\n"
             "for line in sys.stdin:\n"
             "    s = line.strip()\n"
             "    if not s: continue\n"
             "    k, v = s.split(None, 1)\n"
             "    if k in ('update', 'recovery_status'):\n"
             "        sys.stderr.write('FORBIDDEN WRITE TO ' + k + '\\n'); sys.exit(9)\n"
             "    kv[k] = v\n"
             "with open(path, 'w') as f:\n"
             "    for k, v in kv.items(): f.write(f'{k}={v}\\n')\n",
             store_path);
    write_exec(set_bin, script);

    cfg.fw_printenv_bin = print_bin;
    cfg.fw_setenv_bin = set_bin;
    cfg.config_path = cfg_path;

    ASSERT_EQ(fw_env_backend_load(&cfg, &env, err, sizeof(err)), 0);
    ASSERT_EQ(env.active_slot, AB_SLOT_2);
    ASSERT_STREQ(env.swu_update, "yes");
    ASSERT_STREQ(env.swu_recovery_status, "idle_sentinel");

    ASSERT_EQ(ab_stage_trial(&env, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)), 0);
    ASSERT_EQ(fw_env_backend_save(&cfg, &env, err, sizeof(err)), 0);

    ASSERT_EQ(fw_env_backend_load(&cfg, &env, err, sizeof(err)), 0);
    ASSERT_EQ(env.active_slot, AB_SLOT_1);
    ASSERT_EQ(env.pending_slot, AB_SLOT_1);
    ASSERT_EQ(env.ab_state, AB_STATE_TRIAL);
    ASSERT_EQ(env.upgrade_available, 1);
    ASSERT_STREQ(env.swu_update, "yes");
    ASSERT_STREQ(env.swu_recovery_status, "idle_sentinel");

    unlink(store_path);
    unlink(print_bin);
    unlink(set_bin);
    rmdir(tmpdir);

    printf("fw_env_backend_test: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
