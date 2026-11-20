#ifndef SGW_R3C_POLICY_H
#define SGW_R3C_POLICY_H

#include <limits.h>
#include <stdint.h>
#include <sys/types.h>

#define R3C_DEFAULT_POLICY_PATH "/etc/smartgateway/r3c-policy.conf"

typedef enum {
    R3C_GW_STRICT_FORWARD = 0,
    R3C_GW_CONTROLLED_LOOPBACK = 1
} r3c_gw_flow_mode_t;

typedef struct {
    uid_t trusted_uid;
    char data_mountpoint[PATH_MAX];
    char data_device[PATH_MAX];
    uint64_t data_min_bytes;
    char app_ota_dir[PATH_MAX];
    char tx_dir[PATH_MAX];
    char pubkey_path[PATH_MAX];
    char slot1_device[PATH_MAX];
    char slot2_device[PATH_MAX];
    char whole_disk_device[PATH_MAX];
    char supervisor_sock[PATH_MAX];
    char supervisor_exe[PATH_MAX];
    char gateway_sock[PATH_MAX];
    char gateway_exe[PATH_MAX];
    char net_iface[32];
    r3c_gw_flow_mode_t gateway_flow_mode;
    int allow_external_degraded;
    int require_sensor_heartbeat;
    int protect_slot2;
    uint32_t boot_cap;
    uint32_t min_sys_version;
    int require_compat_manifest;
    uint64_t stable_ms;
    uint64_t max_gap_ms;
    uint64_t deadline_ms;
    uint64_t max_evidence_age_ms;
} r3c_policy_t;

void r3c_policy_init_default(r3c_policy_t *p, uid_t owner);

/* Load and authenticate an owner-protected policy file. Rejects missing/untrusted/unknown keys. */
int r3c_policy_load(const char *path, uid_t expected_owner,
                    r3c_policy_t *out, char *err, size_t errlen);

#endif
