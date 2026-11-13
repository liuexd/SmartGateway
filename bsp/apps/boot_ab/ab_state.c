#include "ab_state.h"

#include <stdio.h>
#include <string.h>

static void set_err(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen > 0) {
        snprintf(errbuf, errlen, "%s", msg ? msg : "unknown error");
    }
}

void ab_env_init_default(ab_env_t *env)
{
    if (!env)
        return;

    memset(env, 0, sizeof(*env));
    env->active_slot = AB_SLOT_2;
    env->last_good_slot = AB_SLOT_2;
    env->pending_slot = AB_SLOT_NONE;
    env->ab_state = AB_STATE_CONFIRMED;
    env->bootlimit = 3;
    env->bootcount = 0;
    env->upgrade_available = 0;
    env->rollback_count = 0;
    snprintf(env->last_error, sizeof(env->last_error), "%s", "none");
    snprintf(env->swu_update, sizeof(env->swu_update), "%s", "yes");
    env->swu_recovery_status[0] = '\0';
}

int ab_slot_parse(const char *s, ab_slot_t *out_slot)
{
    if (!s || !out_slot)
        return -1;

    if (strcmp(s, "1") == 0) {
        *out_slot = AB_SLOT_1;
        return 0;
    }
    if (strcmp(s, "2") == 0) {
        *out_slot = AB_SLOT_2;
        return 0;
    }
    if (strcmp(s, "none") == 0 || strcmp(s, "") == 0) {
        *out_slot = AB_SLOT_NONE;
        return 0;
    }
    return -1;
}

const char *ab_slot_str(ab_slot_t slot)
{
    switch (slot) {
    case AB_SLOT_1:
        return "1";
    case AB_SLOT_2:
        return "2";
    case AB_SLOT_NONE:
    default:
        return "none";
    }
}

const char *ab_slot_device(ab_slot_t slot)
{
    switch (slot) {
    case AB_SLOT_1:
        return "/dev/mmcblk1p1";
    case AB_SLOT_2:
        return "/dev/mmcblk1p2";
    default:
        return NULL;
    }
}

ab_slot_t ab_slot_other(ab_slot_t slot)
{
    if (slot == AB_SLOT_1)
        return AB_SLOT_2;
    if (slot == AB_SLOT_2)
        return AB_SLOT_1;
    return AB_SLOT_NONE;
}

int ab_state_parse(const char *s, ab_state_kind_t *out_state)
{
    if (!s || !out_state)
        return -1;

    if (strcmp(s, "confirmed") == 0) {
        *out_state = AB_STATE_CONFIRMED;
        return 0;
    }
    if (strcmp(s, "trial") == 0) {
        *out_state = AB_STATE_TRIAL;
        return 0;
    }
    if (strcmp(s, "rolled_back") == 0) {
        *out_state = AB_STATE_ROLLED_BACK;
        return 0;
    }
    *out_state = AB_STATE_INVALID;
    return -1;
}

const char *ab_state_str(ab_state_kind_t state)
{
    switch (state) {
    case AB_STATE_CONFIRMED:
        return "confirmed";
    case AB_STATE_TRIAL:
        return "trial";
    case AB_STATE_ROLLED_BACK:
        return "rolled_back";
    case AB_STATE_INVALID:
    default:
        return "invalid";
    }
}

int ab_is_swupdate_reserved_var(const char *var_name)
{
    if (!var_name)
        return 0;
    return (strcmp(var_name, "update") == 0 ||
            strcmp(var_name, "recovery_status") == 0);
}

static int is_valid_boot_slot(ab_slot_t slot)
{
    return (slot == AB_SLOT_1 || slot == AB_SLOT_2);
}

int ab_env_validate(const ab_env_t *env, char *errbuf, size_t errlen)
{
    if (!env) {
        set_err(errbuf, errlen, "null env");
        return -1;
    }
    if (!is_valid_boot_slot(env->active_slot)) {
        set_err(errbuf, errlen, "invalid sgw_active_slot");
        return -1;
    }
    if (!is_valid_boot_slot(env->last_good_slot)) {
        set_err(errbuf, errlen, "invalid sgw_last_good_slot");
        return -1;
    }
    if (env->bootlimit == 0 || env->bootlimit > 10) {
        set_err(errbuf, errlen, "bootlimit out of range [1..10]");
        return -1;
    }
    if (env->upgrade_available != 0 && env->upgrade_available != 1) {
        set_err(errbuf, errlen, "upgrade_available must be 0 or 1");
        return -1;
    }
    if (env->ab_state == AB_STATE_INVALID) {
        set_err(errbuf, errlen, "invalid sgw_ab_state");
        return -1;
    }
    if (env->ab_state == AB_STATE_TRIAL) {
        if (env->upgrade_available != 1) {
            set_err(errbuf, errlen, "trial state requires upgrade_available=1");
            return -1;
        }
        if (!is_valid_boot_slot(env->pending_slot) ||
            env->pending_slot != env->active_slot) {
            set_err(errbuf, errlen, "trial state requires pending_slot == active_slot");
            return -1;
        }
        if (env->active_slot == env->last_good_slot) {
            set_err(errbuf, errlen, "trial active_slot cannot equal last_good_slot");
            return -1;
        }
    } else {
        if (env->upgrade_available != 0) {
            set_err(errbuf, errlen, "non-trial state requires upgrade_available=0");
            return -1;
        }
        if (env->pending_slot != AB_SLOT_NONE) {
            set_err(errbuf, errlen, "non-trial state requires pending_slot=none");
            return -1;
        }
    }
    return 0;
}

int ab_stage_trial(ab_env_t *env,
                   ab_slot_t target_slot,
                   ab_slot_t running_slot,
                   char *errbuf,
                   size_t errlen)
{
    if (!env) {
        set_err(errbuf, errlen, "null env");
        return -1;
    }
    if (!is_valid_boot_slot(target_slot)) {
        set_err(errbuf, errlen, "invalid target slot");
        return -1;
    }
    if (!is_valid_boot_slot(running_slot)) {
        set_err(errbuf, errlen, "invalid running slot");
        return -1;
    }
    if (target_slot == running_slot) {
        set_err(errbuf, errlen, "refusing to stage currently running slot");
        return -1;
    }
    if (strcmp(env->swu_recovery_status, "in_progress") == 0) {
        set_err(errbuf, errlen, "SWUpdate recovery_status=in_progress active");
        return -1;
    }
    if (env->ab_state == AB_STATE_TRIAL || env->upgrade_available != 0) {
        set_err(errbuf, errlen, "trial upgrade already in progress");
        return -1;
    }
    if (env->last_good_slot != running_slot) {
        set_err(errbuf, errlen, "running slot is not confirmed last_good_slot");
        return -1;
    }

    env->active_slot = target_slot;
    env->pending_slot = target_slot;
    env->ab_state = AB_STATE_TRIAL;
    env->upgrade_available = 1;
    env->bootcount = 0;
    snprintf(env->last_error, sizeof(env->last_error), "%s", "none");
    return 0;
}

int ab_uboot_boot_step(ab_env_t *env,
                       ab_slot_t *out_boot_slot,
                       int *out_env_written,
                       int *out_rolled_back,
                       int *out_watchdog_armed)
{
    unsigned long bc;
    int wrote = 0;
    int rolled = 0;
    ab_slot_t selected;

    if (!env || !out_boot_slot)
        return -1;

    /* Mirrors U-Boot 2017.03 autoboot.c + drivers/bootcount/bootcount_env.c */
    bc = env->upgrade_available ? env->bootcount : 0;
    bc++;

    if (env->upgrade_available) {
        env->bootcount = bc;
        wrote = 1;
    }

    if (env->bootlimit > 0 && bc > env->bootlimit) {
        /* Mirrors altbootcmd */
        ab_slot_t safe = is_valid_boot_slot(env->last_good_slot)
                             ? env->last_good_slot
                             : AB_SLOT_2;
        env->active_slot = safe;
        env->last_good_slot = safe;
        env->pending_slot = AB_SLOT_NONE;
        env->ab_state = AB_STATE_ROLLED_BACK;
        snprintf(env->last_error, sizeof(env->last_error), "%s", "bootlimit_exceeded");
        env->rollback_count++;
        env->upgrade_available = 0;
        env->bootcount = 0;
        wrote = 1;
        rolled = 1;
    }

    /* Mirrors sgw_select_slot */
    if (is_valid_boot_slot(env->active_slot)) {
        selected = env->active_slot;
    } else if (is_valid_boot_slot(env->last_good_slot)) {
        selected = env->last_good_slot;
        env->active_slot = selected;
    } else {
        selected = AB_SLOT_2;
        env->active_slot = selected;
        env->last_good_slot = selected;
    }

    *out_boot_slot = selected;
    if (out_env_written)
        *out_env_written = wrote;
    if (out_rolled_back)
        *out_rolled_back = rolled;
    if (out_watchdog_armed)
        *out_watchdog_armed = 1;

    return 0;
}

int ab_confirm_trial(ab_env_t *env,
                     ab_slot_t running_slot,
                     int health_ok,
                     char *errbuf,
                     size_t errlen)
{
    if (!env) {
        set_err(errbuf, errlen, "null env");
        return -1;
    }
    if (!health_ok) {
        set_err(errbuf, errlen, "health check failed, refusing confirm");
        return -1;
    }
    if (!is_valid_boot_slot(running_slot)) {
        set_err(errbuf, errlen, "invalid running slot");
        return -1;
    }
    if (env->ab_state != AB_STATE_TRIAL || env->upgrade_available != 1) {
        set_err(errbuf, errlen, "no pending trial boot to confirm");
        return -1;
    }
    if (running_slot != env->active_slot || running_slot != env->pending_slot) {
        set_err(errbuf, errlen, "running slot does not match pending trial slot");
        return -1;
    }

    env->last_good_slot = running_slot;
    env->pending_slot = AB_SLOT_NONE;
    env->ab_state = AB_STATE_CONFIRMED;
    env->upgrade_available = 0;
    env->bootcount = 0;
    snprintf(env->last_error, sizeof(env->last_error), "%s", "none");
    return 0;
}

int ab_force_rollback(ab_env_t *env,
                      const char *reason,
                      char *errbuf,
                      size_t errlen)
{
    ab_slot_t safe;

    if (!env) {
        set_err(errbuf, errlen, "null env");
        return -1;
    }
    safe = is_valid_boot_slot(env->last_good_slot) ? env->last_good_slot : AB_SLOT_2;
    env->active_slot = safe;
    env->last_good_slot = safe;
    env->pending_slot = AB_SLOT_NONE;
    env->ab_state = AB_STATE_ROLLED_BACK;
    env->upgrade_available = 0;
    env->bootcount = 0;
    env->rollback_count++;
    snprintf(env->last_error, sizeof(env->last_error), "%s",
             (reason && reason[0]) ? reason : "manual_rollback");
    return 0;
}

int ab_watchdog_trial_expired(uint64_t elapsed_ms,
                              uint64_t trial_budget_ms,
                              int health_confirmed)
{
    if (health_confirmed)
        return 0;
    return elapsed_ms >= trial_budget_ms ? 1 : 0;
}