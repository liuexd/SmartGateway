#ifndef SGW_AB_HEALTH_H
#define SGW_AB_HEALTH_H

#include "ab_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *proc_cmdline_path;
    const char *proc_mounts_path;
    const char *supervisor_run_dir;
    const char *sysroot_prefix;
    int supervisor_timeout_ms;
    int verify_supervisor_pid;
} ab_health_cfg_t;

void ab_health_cfg_init(ab_health_cfg_t *cfg);

int ab_detect_running_slot(const ab_health_cfg_t *cfg,
                           ab_slot_t *out_slot,
                           char *errbuf,
                           size_t errlen);

int ab_verify_system_health(const ab_health_cfg_t *cfg,
                            const ab_env_t *env,
                            ab_slot_t *out_running_slot,
                            unsigned long long *out_supervisor_pid,
                            char *errbuf,
                            size_t errlen);

#ifdef __cplusplus
}
#endif

#endif /* SGW_AB_HEALTH_H */
