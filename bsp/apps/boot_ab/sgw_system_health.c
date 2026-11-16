#include "ab_health.h"
#include "fw_env_backend.h"
#include "health_supervisor.h"
#include "system_health.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

/*
 * Diagnostic only: no updater, no fw_setenv and no way to confirm a slot.
 * The full business/device/DATA providers and authenticated transaction ID
 * are not integrated. Missing evidence must remain UNKNOWN.
 */
int main(int argc, char **argv)
{
    ab_health_cfg_t health;
    fw_env_backend_cfg_t fw = {
        "/usr/sbin/fw_printenv", "/usr/sbin/fw_setenv", "/etc/fw_env.config"
    };
    ab_env_t env;
    ab_slot_t slot = AB_SLOT_NONE;
    pid_t gateway_pid = 0;
    char err[256] = {0};
    int root_ok, trial_ok = 0, supervisor_ok;

    if (argc != 2 || strcmp(argv[1], "--once") != 0) {
        fprintf(stderr, "Usage: %s --once\n", argv[0]);
        return 2;
    }
    ab_health_cfg_init(&health);
    /* Do not accept test paths or values from an inherited environment. */
    health.proc_cmdline_path = "/proc/cmdline";
    health.proc_mounts_path = "/proc/mounts";
    health.supervisor_run_dir = "/opt/smartgateway/run";
    health.sysroot_prefix = "";
    root_ok = ab_detect_running_slot(&health, &slot, err, sizeof(err)) == 0;
    if (root_ok && fw_env_backend_load(&fw, &env, err, sizeof(err)) == 0 &&
        ab_env_validate(&env, err, sizeof(err)) == 0 &&
        env.ab_state == AB_STATE_TRIAL && env.upgrade_available == 1 &&
        env.active_slot == slot && env.pending_slot == slot &&
        env.last_good_slot != slot && env.bootcount > 0) {
        trial_ok = 1;
    }

    supervisor_ok = sgw_health_supervisor_status(
        "/opt/smartgateway/run/supervisor.sock",
        "/opt/smartgateway/bin/service_supervisor",
        "/opt/smartgateway/current/smart_gateway",
        0, 2000, &gateway_pid) == 0;

    printf("root_identity=%s\n", root_ok ? "HEALTHY" : "UNKNOWN");
    printf("trial_observed=%s\n", trial_ok ? "yes" : "UNKNOWN");
    printf("system=UNKNOWN\n");
    printf("supervisor=%s\n", supervisor_ok ? "HEALTHY" : "UNKNOWN");
    if (supervisor_ok) printf("managed_gateway_pid=%ld\n", (long)gateway_pid);
    printf("gateway_business=UNKNOWN\n");
    printf("gpio=UNKNOWN\n");
    printf("ap3216_i2c=UNKNOWN\n");
    printf("local_network=UNKNOWN\n");
    printf("data=UNKNOWN\n");
    printf("external=UNKNOWN\n");
    printf("overall=%s\n", sgw_health_state_name(SGW_HEALTH_UNKNOWN));
    printf("confirmation=DISABLED\n");
    return 3;
}
