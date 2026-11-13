#ifndef SGW_AB_STATE_H
#define SGW_AB_STATE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AB_SLOT_NONE = 0,
    AB_SLOT_1 = 1,
    AB_SLOT_2 = 2
} ab_slot_t;

typedef enum {
    AB_STATE_CONFIRMED = 0,
    AB_STATE_TRIAL,
    AB_STATE_ROLLED_BACK,
    AB_STATE_INVALID
} ab_state_kind_t;

typedef struct {
    ab_slot_t active_slot;
    ab_slot_t last_good_slot;
    ab_slot_t pending_slot;
    ab_state_kind_t ab_state;
    unsigned long bootlimit;
    unsigned long bootcount;
    int upgrade_available;
    unsigned long rollback_count;
    char last_error[64];
    /* Read-only mirrors of SWUpdate-reserved env vars (never written by A/B). */
    char swu_update[32];
    char swu_recovery_status[32];
} ab_env_t;

void ab_env_init_default(ab_env_t *env);

int ab_slot_parse(const char *s, ab_slot_t *out_slot);
const char *ab_slot_str(ab_slot_t slot);
const char *ab_slot_device(ab_slot_t slot);
ab_slot_t ab_slot_other(ab_slot_t slot);

int ab_state_parse(const char *s, ab_state_kind_t *out_state);
const char *ab_state_str(ab_state_kind_t state);

/* Returns 1 if var_name is reserved by SWUpdate ('update' or 'recovery_status'). */
int ab_is_swupdate_reserved_var(const char *var_name);

int ab_env_validate(const ab_env_t *env, char *errbuf, size_t errlen);

int ab_stage_trial(ab_env_t *env,
                   ab_slot_t target_slot,
                   ab_slot_t running_slot,
                   char *errbuf,
                   size_t errlen);

int ab_uboot_boot_step(ab_env_t *env,
                       ab_slot_t *out_boot_slot,
                       int *out_env_written,
                       int *out_rolled_back,
                       int *out_watchdog_armed);

int ab_confirm_trial(ab_env_t *env,
                     ab_slot_t running_slot,
                     int health_ok,
                     char *errbuf,
                     size_t errlen);

int ab_force_rollback(ab_env_t *env,
                      const char *reason,
                      char *errbuf,
                      size_t errlen);

int ab_watchdog_trial_expired(uint64_t elapsed_ms,
                              uint64_t trial_budget_ms,
                              int health_confirmed);

#ifdef __cplusplus
}
#endif

#endif /* SGW_AB_STATE_H */