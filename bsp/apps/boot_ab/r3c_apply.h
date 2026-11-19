#ifndef SGW_R3C_APPLY_H
#define SGW_R3C_APPLY_H

#include "ab_health.h"
#include "ab_state.h"
#include "fw_env_backend.h"
#include "r3c_policy.h"

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned int target_slot;
    unsigned int expected_target;
    int has_expected_target;
    const char *bundle_dir;
    const char *mountinfo_path;      /* NULL in production -> /proc/self/mountinfo */
    const char *target_dev_override; /* NULL in production -> /dev/mmcblk1p{1,2} */
    int allow_regular_file_target;   /* 0 in production */
    int allow_host_slot_sim;         /* 0 in production */
} r3c_apply_opts_t;

int r3c_coordinator_apply_bundle(const r3c_policy_t *policy,
                                 const fw_env_backend_cfg_t *fw_cfg,
                                 const ab_health_cfg_t *hcfg,
                                 const r3c_apply_opts_t *opts,
                                 char out_txid[65],
                                 ab_slot_t *out_last_good,
                                 char *errbuf,
                                 size_t errlen);

#ifdef __cplusplus
}
#endif

#endif
