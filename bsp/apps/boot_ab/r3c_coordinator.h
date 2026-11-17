#ifndef SGW_R3C_COORDINATOR_H
#define SGW_R3C_COORDINATOR_H

#include "ab_health.h"
#include "ab_state.h"
#include "fw_env_backend.h"
#include "r3c_policy.h"
#include "system_health.h"

#include <stddef.h>
#include <stdint.h>

/*
 * Runtime backend overrides used ONLY by the isolated host test harness.
 * Production sgw_bootctl passes ops=NULL, enforcing real kernel /proc, /sys,
 * real ext4 block mounts, real slot devices, and /etc/smartgateway/r3c-policy.conf.
 */
typedef struct {
    const char *boot_id_path;
    const char *mountinfo_path;
    const char *sysfs_block_root;
    const char *expected_data_fstype;
    const char *slot_image_override;      /* optional path to slot image file in harness */
    int allow_fuse_data_bypass_blk;       /* ONLY when host harness uses non-root fuse2fs */
    int allow_host_slot_sim;              /* ONLY when host lacks /dev/mmcblk1p1|p2 */
    const char *gpio_dev_node;
    const char *gpio_misc_dev;
    const char *gpio_stats_path;
    const char *gpio_driver_link;
    const char *ap3216_als_path;
    const char *ap3216_stats_path;
    const char *ap3216_driver_link;
    sgw_health_state_t (*network_probe_fn)(const char *iface, void *ctx);
    sgw_health_state_t (*sensor_hb_fn)(void *ctx);
    void *ctx;
} r3c_runtime_ops_t;

typedef struct {
    char transaction_id[65];
    char boot_id[37];
    ab_slot_t running_slot;
    ab_slot_t last_good_slot;
    unsigned long bootcount;
    sgw_health_state_t overall_state;
    unsigned int samples;
} r3c_confirm_result_t;

int r3c_coordinator_confirm(const r3c_policy_t *policy,
                            const fw_env_backend_cfg_t *fw_cfg,
                            const ab_health_cfg_t *hcfg,
                            const r3c_runtime_ops_t *ops,
                            r3c_confirm_result_t *out,
                            char *errbuf,
                            size_t errlen);

#endif
