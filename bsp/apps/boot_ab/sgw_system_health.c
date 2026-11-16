#include "ab_health.h"
#include "fw_env_backend.h"
#include "health_supervisor.h"
#include "health_gateway.h"
#include "health_devices.h"
#include "health_network.h"
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
    pid_t gateway_pid = 0, rechecked_pid = 0;
    unsigned long long gateway_start = 0, rechecked_start = 0;
    char err[256] = {0};
    int root_ok, trial_ok = 0, supervisor_ok, gateway_components_ok = 0;
    unsigned int als_raw = 0;
    sgw_health_state_t gpio, als, sensor, network;

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
        0, 2000, &gateway_pid, &gateway_start) == 0;
    if (supervisor_ok &&
        sgw_health_gateway_probe("/opt/smartgateway/run/gateway-health.sock",
                                 0, gateway_pid, gateway_start, 1000) == 0 &&
        sgw_health_supervisor_status(
            "/opt/smartgateway/run/supervisor.sock",
            "/opt/smartgateway/bin/service_supervisor",
            "/opt/smartgateway/current/smart_gateway",
            0, 2000, &rechecked_pid, &rechecked_start) == 0 &&
        rechecked_pid == gateway_pid && rechecked_start == gateway_start) {
        gateway_components_ok = 1;
    }
    gpio = sgw_health_gpio_inventory(300);
    als = sgw_health_ap3216_read(&als_raw, 300);
    /* No authenticated NODE04 heartbeat source is integrated yet. */
    sensor = sgw_health_sensor_combine(als, SGW_HEALTH_UNKNOWN);
    network = sgw_health_network_policy();

    printf("root_identity=%s\n", root_ok ? "HEALTHY" : "UNKNOWN");
    printf("trial_observed=%s\n", trial_ok ? "yes" : "UNKNOWN");
    printf("system=UNKNOWN\n");
    printf("supervisor=%s\n", supervisor_ok ? "HEALTHY" : "UNKNOWN");
    if (supervisor_ok) printf("managed_gateway_pid=%ld\n", (long)gateway_pid);
    printf("gateway_components=%s\n", gateway_components_ok ? "HEALTHY" : "UNKNOWN");
    printf("gateway_business=UNKNOWN\n");
    printf("gpio=%s\n", sgw_health_state_name(gpio));
    printf("ap3216_direct=%s\n", sgw_health_state_name(als));
    if (als == SGW_HEALTH_HEALTHY) printf("als_raw=%u\n", als_raw);
    printf("ap3216_i2c=%s\n", sgw_health_state_name(sensor));
    printf("local_network=%s\n", sgw_health_state_name(network));
    printf("data=UNKNOWN\n");
    printf("external=UNKNOWN\n");
    printf("overall=%s\n", sgw_health_state_name(
        gpio == SGW_HEALTH_UNHEALTHY || sensor == SGW_HEALTH_UNHEALTHY ||
        network == SGW_HEALTH_UNHEALTHY ?
        SGW_HEALTH_UNHEALTHY : SGW_HEALTH_UNKNOWN));
    printf("confirmation=DISABLED\n");
    return 3;
}
