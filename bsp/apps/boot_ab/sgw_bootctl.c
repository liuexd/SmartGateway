#include "ab_state.h"
#include "ab_health.h"
#include "fw_env_backend.h"
#include "r3c_coordinator.h"
#include "r3c_policy.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void print_usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [-c fw_env.config] [--run-dir dir] [--cmdline path] [--mounts path] [--sysroot dir] <command> [options]\n"
            "Commands:\n"
            "  status [--json]\n"
            "  validate\n"
            "  running-slot\n"
            "  health-check\n"
            "  stage --target <1|2> [--running <1|2>]\n"
            "  confirm [--running <1|2>]\n"
            "  rollback [--reason <text>]\n"
            "  watchdog-check --elapsed-ms <n> --budget-ms <n> [--health-confirmed <0|1>]\n",
            prog);
}

static int parse_u64(const char *s, uint64_t *out)
{
    char *end = NULL;
    unsigned long long v;

    if (!s || !s[0] || !out)
        return -1;
    errno = 0;
    v = strtoull(s, &end, 10);
    if (errno != 0 || !end || *end != '\0')
        return -1;
    *out = (uint64_t)v;
    return 0;
}

static void print_status(const ab_env_t *env, int valid, const char *err, int as_json)
{
    const char *dev = ab_slot_device(env->active_slot);

    if (as_json) {
        printf("{\"valid\":%s,\"error\":\"%s\","
               "\"sgw_active_slot\":\"%s\",\"device\":\"%s\","
               "\"sgw_last_good_slot\":\"%s\",\"sgw_pending_slot\":\"%s\","
               "\"sgw_ab_state\":\"%s\",\"bootlimit\":%lu,"
               "\"bootcount\":%lu,\"upgrade_available\":%d,"
               "\"sgw_rollback_count\":%lu,\"sgw_last_error\":\"%s\","
               "\"swu_update\":\"%s\",\"swu_recovery_status\":\"%s\"}\n",
               valid ? "true" : "false",
               err[0] ? err : "none",
               ab_slot_str(env->active_slot),
               dev ? dev : "none",
               ab_slot_str(env->last_good_slot),
               ab_slot_str(env->pending_slot),
               ab_state_str(env->ab_state),
               env->bootlimit,
               env->bootcount,
               env->upgrade_available,
               env->rollback_count,
               env->last_error,
               env->swu_update,
               env->swu_recovery_status);
    } else {
        printf("valid=%s\n"
               "sgw_active_slot=%s\n"
               "device=%s\n"
               "sgw_last_good_slot=%s\n"
               "sgw_pending_slot=%s\n"
               "sgw_ab_state=%s\n"
               "bootlimit=%lu\n"
               "bootcount=%lu\n"
               "upgrade_available=%d\n"
               "sgw_rollback_count=%lu\n"
               "sgw_last_error=%s\n"
               "update=%s\n"
               "recovery_status=%s\n",
               valid ? "1" : "0",
               ab_slot_str(env->active_slot),
               dev ? dev : "none",
               ab_slot_str(env->last_good_slot),
               ab_slot_str(env->pending_slot),
               ab_state_str(env->ab_state),
               env->bootlimit,
               env->bootcount,
               env->upgrade_available,
               env->rollback_count,
               env->last_error,
               env->swu_update,
               env->swu_recovery_status);
    }
}

int main(int argc, char **argv)
{
    fw_env_backend_cfg_t cfg;
    ab_health_cfg_t hcfg;
    ab_env_t env;
    char errbuf[256] = {0};
    int idx = 1;
    const char *cmd;

    fw_env_backend_cfg_from_env(&cfg);
    ab_health_cfg_init(&hcfg);

    while (idx < argc) {
        if ((strcmp(argv[idx], "-c") == 0 || strcmp(argv[idx], "--config") == 0) &&
            idx + 1 < argc) {
            cfg.config_path = argv[idx + 1];
            idx += 2;
        } else if (strcmp(argv[idx], "--run-dir") == 0 && idx + 1 < argc) {
            hcfg.supervisor_run_dir = argv[idx + 1];
            idx += 2;
        } else if (strcmp(argv[idx], "--cmdline") == 0 && idx + 1 < argc) {
            hcfg.proc_cmdline_path = argv[idx + 1];
            idx += 2;
        } else if (strcmp(argv[idx], "--mounts") == 0 && idx + 1 < argc) {
            hcfg.proc_mounts_path = argv[idx + 1];
            idx += 2;
        } else if (strcmp(argv[idx], "--sysroot") == 0 && idx + 1 < argc) {
            hcfg.sysroot_prefix = argv[idx + 1];
            idx += 2;
        } else {
            break;
        }
    }

    if (idx >= argc) {
        print_usage(argv[0]);
        return 2;
    }
    cmd = argv[idx++];

    if (strcmp(cmd, "watchdog-check") == 0) {
        uint64_t elapsed = 0;
        uint64_t budget = 0;
        int has_elapsed = 0;
        int has_budget = 0;
        int confirmed = 0;

        while (idx < argc) {
            if (strcmp(argv[idx], "--elapsed-ms") == 0 && idx + 1 < argc) {
                if (parse_u64(argv[idx + 1], &elapsed) != 0)
                    return 2;
                has_elapsed = 1;
                idx += 2;
            } else if (strcmp(argv[idx], "--budget-ms") == 0 && idx + 1 < argc) {
                if (parse_u64(argv[idx + 1], &budget) != 0)
                    return 2;
                has_budget = 1;
                idx += 2;
            } else if (strcmp(argv[idx], "--health-confirmed") == 0 && idx + 1 < argc) {
                confirmed = atoi(argv[idx + 1]) ? 1 : 0;
                idx += 2;
            } else {
                print_usage(argv[0]);
                return 2;
            }
        }
        if (!has_elapsed || !has_budget) {
            print_usage(argv[0]);
            return 2;
        }
        if (ab_watchdog_trial_expired(elapsed, budget, confirmed)) {
            printf("expired=1\n");
            return 3;
        }
        printf("expired=0\n");
        return 0;
    }

    if (strcmp(cmd, "running-slot") == 0) {
        ab_slot_t detected = AB_SLOT_NONE;
        if (ab_detect_running_slot(&hcfg, &detected, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "RUNNING SLOT ERROR: %s\n", errbuf);
            return 1;
        }
        printf("running_slot=%s device=%s\n",
               ab_slot_str(detected), ab_slot_device(detected));
        return 0;
    }

    if (fw_env_backend_load(&cfg, &env, errbuf, sizeof(errbuf)) != 0) {
        fprintf(stderr, "ERROR: %s\n", errbuf);
        return 1;
    }

    if (strcmp(cmd, "status") == 0) {
        int as_json = 0;
        int valid;

        while (idx < argc) {
            if (strcmp(argv[idx], "--json") == 0) {
                as_json = 1;
                idx++;
            } else {
                print_usage(argv[0]);
                return 2;
            }
        }
        valid = (ab_env_validate(&env, errbuf, sizeof(errbuf)) == 0);
        print_status(&env, valid, errbuf, as_json);
        return valid ? 0 : 1;
    }

    if (strcmp(cmd, "validate") == 0) {
        if (ab_env_validate(&env, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "INVALID: %s\n", errbuf);
            return 1;
        }
        printf("VALID slot=%s device=%s state=%s\n",
               ab_slot_str(env.active_slot),
               ab_slot_device(env.active_slot),
               ab_state_str(env.ab_state));
        return 0;
    }

    if (strcmp(cmd, "health-check") == 0) {
        ab_slot_t detected = AB_SLOT_NONE;
        unsigned long long svc_pid = 0;

        if (ab_verify_system_health(&hcfg, &env, &detected, &svc_pid,
                                    errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "HEALTH FAILED: %s\n", errbuf);
            return 1;
        }
        printf("HEALTH_OK slot=%s device=%s supervisor_service_pid=%llu\n",
               ab_slot_str(detected), ab_slot_device(detected), svc_pid);
        return 0;
    }

    if (strcmp(cmd, "stage") == 0) {
        ab_slot_t target = AB_SLOT_NONE;
        ab_slot_t caller_running = AB_SLOT_NONE;
        ab_slot_t detected_running = AB_SLOT_NONE;
        int has_caller_running = 0;

        while (idx < argc) {
            if (strcmp(argv[idx], "--target") == 0 && idx + 1 < argc) {
                if (ab_slot_parse(argv[idx + 1], &target) != 0)
                    return 2;
                idx += 2;
            } else if (strcmp(argv[idx], "--running") == 0 && idx + 1 < argc) {
                if (ab_slot_parse(argv[idx + 1], &caller_running) != 0)
                    return 2;
                has_caller_running = 1;
                idx += 2;
            } else {
                print_usage(argv[0]);
                return 2;
            }
        }
        if (ab_detect_running_slot(&hcfg, &detected_running, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "STAGE ERROR: %s\n", errbuf);
            return 1;
        }
        if (has_caller_running && caller_running != detected_running) {
            fprintf(stderr,
                    "STAGE ERROR: caller --running %s disagrees with verified OS root slot %s\n",
                    ab_slot_str(caller_running), ab_slot_str(detected_running));
            return 1;
        }
        if (ab_stage_trial(&env, target, detected_running, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "STAGE ERROR: %s\n", errbuf);
            return 1;
        }
        if (fw_env_backend_save(&cfg, &env, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "SAVE ERROR: %s\n", errbuf);
            return 1;
        }
        printf("STAGED target=%s device=%s state=%s upgrade_available=%d\n",
               ab_slot_str(env.pending_slot),
               ab_slot_device(env.pending_slot),
               ab_state_str(env.ab_state),
               env.upgrade_available);
        return 0;
    }

    if (strcmp(cmd, "confirm") == 0) {
        ab_slot_t caller_running = AB_SLOT_NONE;
        int has_caller_running = 0;
        r3c_policy_t r3c_pol;
        r3c_confirm_result_t res;

        while (idx < argc) {
            if (strcmp(argv[idx], "--running") == 0 && idx + 1 < argc) {
                if (ab_slot_parse(argv[idx + 1], &caller_running) != 0)
                    return 2;
                has_caller_running = 1;
                idx += 2;
            } else {
                print_usage(argv[0]);
                return 2;
            }
        }
        /* Production confirm: root-owned /etc/smartgateway/r3c-policy.conf, zero CLI/env bypasses */
        if (r3c_policy_load(R3C_DEFAULT_POLICY_PATH, 0, &r3c_pol, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "CONFIRM REJECTED: %s\n", errbuf);
            return 1;
        }
        if (r3c_coordinator_confirm(&r3c_pol, &cfg, &hcfg, NULL, &res, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "CONFIRM REJECTED: %s\n", errbuf);
            return 1;
        }
        if (has_caller_running && caller_running != res.running_slot) {
            fprintf(stderr, "CONFIRM REJECTED: caller --running disagrees with verified slot\n");
            return 1;
        }
        printf("CONFIRMED active=%s last_good=%s state=confirmed tx=%s boot_id=%s health=%s\n",
               ab_slot_str(res.running_slot),
               ab_slot_str(res.last_good_slot),
               res.transaction_id,
               res.boot_id,
               sgw_health_state_name(res.overall_state));
        return 0;
    }

    if (strcmp(cmd, "rollback") == 0) {
        const char *reason = "manual_rollback";

        while (idx < argc) {
            if (strcmp(argv[idx], "--reason") == 0 && idx + 1 < argc) {
                reason = argv[idx + 1];
                idx += 2;
            } else {
                print_usage(argv[0]);
                return 2;
            }
        }
        if (ab_force_rollback(&env, reason, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "ROLLBACK ERROR: %s\n", errbuf);
            return 1;
        }
        if (fw_env_backend_save(&cfg, &env, errbuf, sizeof(errbuf)) != 0) {
            fprintf(stderr, "SAVE ERROR: %s\n", errbuf);
            return 1;
        }
        printf("ROLLED_BACK active=%s rollback_count=%lu reason=%s\n",
               ab_slot_str(env.active_slot),
               env.rollback_count,
               env.last_error);
        return 0;
    }

    print_usage(argv[0]);
    return 2;
}
