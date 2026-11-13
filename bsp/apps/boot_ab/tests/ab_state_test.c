#include "ab_state.h"

#include <stdio.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define ASSERT_TRUE(cond) do {     if (!(cond)) {         fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);         g_fail++;     } else {         g_pass++;     } } while (0)

#define ASSERT_EQ(a, b) ASSERT_TRUE((a) == (b))
#define ASSERT_STREQ(a, b) ASSERT_TRUE(strcmp((a), (b)) == 0)

static void test_defaults_and_helpers(void)
{
    ab_env_t env;
    char err[128] = {0};
    ab_slot_t slot = AB_SLOT_NONE;
    ab_state_kind_t st = AB_STATE_INVALID;

    ab_env_init_default(&env);
    ASSERT_EQ(env.active_slot, AB_SLOT_2);
    ASSERT_EQ(env.last_good_slot, AB_SLOT_2);
    ASSERT_EQ(env.pending_slot, AB_SLOT_NONE);
    ASSERT_EQ(env.ab_state, AB_STATE_CONFIRMED);
    ASSERT_EQ(env.bootlimit, 3UL);
    ASSERT_EQ(env.bootcount, 0UL);
    ASSERT_EQ(env.upgrade_available, 0);
    ASSERT_STREQ(env.swu_update, "yes");
    ASSERT_EQ(ab_env_validate(&env, err, sizeof(err)), 0);

    ASSERT_EQ(ab_slot_parse("1", &slot), 0);
    ASSERT_EQ(slot, AB_SLOT_1);
    ASSERT_STREQ(ab_slot_device(slot), "/dev/mmcblk1p1");
    ASSERT_EQ(ab_slot_other(slot), AB_SLOT_2);

    ASSERT_EQ(ab_slot_parse("2", &slot), 0);
    ASSERT_EQ(slot, AB_SLOT_2);
    ASSERT_STREQ(ab_slot_device(slot), "/dev/mmcblk1p2");
    ASSERT_EQ(ab_slot_other(slot), AB_SLOT_1);

    ASSERT_EQ(ab_slot_parse("none", &slot), 0);
    ASSERT_EQ(slot, AB_SLOT_NONE);
    ASSERT_TRUE(ab_slot_parse("3", &slot) != 0);

    ASSERT_EQ(ab_state_parse("confirmed", &st), 0);
    ASSERT_EQ(st, AB_STATE_CONFIRMED);
    ASSERT_EQ(ab_state_parse("trial", &st), 0);
    ASSERT_EQ(st, AB_STATE_TRIAL);
    ASSERT_EQ(ab_state_parse("rolled_back", &st), 0);
    ASSERT_EQ(st, AB_STATE_ROLLED_BACK);
    ASSERT_TRUE(ab_state_parse("bogus", &st) != 0);

    ASSERT_EQ(ab_is_swupdate_reserved_var("update"), 1);
    ASSERT_EQ(ab_is_swupdate_reserved_var("recovery_status"), 1);
    ASSERT_EQ(ab_is_swupdate_reserved_var("sgw_active_slot"), 0);
}

static void test_stage_guards_and_confirm_bidirectional(void)
{
    ab_env_t env;
    char err[128] = {0};
    ab_slot_t boot_slot = AB_SLOT_NONE;
    int env_written = 0, rolled_back = 0, wdt = 0;

    ab_env_init_default(&env);
    ASSERT_TRUE(ab_stage_trial(&env, AB_SLOT_2, AB_SLOT_2, err, sizeof(err)) != 0);

    snprintf(env.swu_recovery_status, sizeof(env.swu_recovery_status), "%s", "in_progress");
    ASSERT_TRUE(ab_stage_trial(&env, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)) != 0);
    env.swu_recovery_status[0] = '\0';

    ASSERT_EQ(ab_stage_trial(&env, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)), 0);
    ASSERT_EQ(ab_env_validate(&env, err, sizeof(err)), 0);
    ASSERT_EQ(env.active_slot, AB_SLOT_1);
    ASSERT_EQ(env.pending_slot, AB_SLOT_1);
    ASSERT_EQ(env.last_good_slot, AB_SLOT_2);
    ASSERT_EQ(env.upgrade_available, 1);
    ASSERT_EQ(env.ab_state, AB_STATE_TRIAL);

    ASSERT_TRUE(ab_stage_trial(&env, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)) != 0);

    ASSERT_EQ(ab_uboot_boot_step(&env, &boot_slot, &env_written, &rolled_back, &wdt), 0);
    ASSERT_EQ(boot_slot, AB_SLOT_1);
    ASSERT_EQ(env_written, 1);
    ASSERT_EQ(rolled_back, 0);
    ASSERT_EQ(wdt, 1);
    ASSERT_EQ(env.bootcount, 1UL);

    ASSERT_TRUE(ab_confirm_trial(&env, AB_SLOT_1, 0, err, sizeof(err)) != 0);
    ASSERT_TRUE(ab_confirm_trial(&env, AB_SLOT_2, 1, err, sizeof(err)) != 0);

    ASSERT_EQ(ab_confirm_trial(&env, AB_SLOT_1, 1, err, sizeof(err)), 0);
    ASSERT_EQ(env.active_slot, AB_SLOT_1);
    ASSERT_EQ(env.last_good_slot, AB_SLOT_1);
    ASSERT_EQ(env.pending_slot, AB_SLOT_NONE);
    ASSERT_EQ(env.ab_state, AB_STATE_CONFIRMED);
    ASSERT_EQ(env.upgrade_available, 0);
    ASSERT_EQ(env.bootcount, 0UL);
    ASSERT_EQ(ab_env_validate(&env, err, sizeof(err)), 0);

    ASSERT_EQ(ab_uboot_boot_step(&env, &boot_slot, &env_written, &rolled_back, &wdt), 0);
    ASSERT_EQ(boot_slot, AB_SLOT_1);
    ASSERT_EQ(env_written, 0);
    ASSERT_EQ(rolled_back, 0);

    ASSERT_EQ(ab_stage_trial(&env, AB_SLOT_2, AB_SLOT_1, err, sizeof(err)), 0);
    ASSERT_EQ(ab_uboot_boot_step(&env, &boot_slot, &env_written, &rolled_back, &wdt), 0);
    ASSERT_EQ(boot_slot, AB_SLOT_2);
    ASSERT_EQ(ab_confirm_trial(&env, AB_SLOT_2, 1, err, sizeof(err)), 0);
    ASSERT_EQ(env.last_good_slot, AB_SLOT_2);
}

static void test_bootlimit_auto_rollback(void)
{
    ab_env_t env;
    char err[128] = {0};
    ab_slot_t boot_slot = AB_SLOT_NONE;
    int env_written = 0, rolled_back = 0, wdt = 0;

    ab_env_init_default(&env);
    ASSERT_EQ(ab_stage_trial(&env, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)), 0);

    for (unsigned long expected = 1; expected <= 3; expected++) {
        ASSERT_EQ(ab_uboot_boot_step(&env, &boot_slot, &env_written, &rolled_back, &wdt), 0);
        ASSERT_EQ(boot_slot, AB_SLOT_1);
        ASSERT_EQ(env.bootcount, expected);
        ASSERT_EQ(rolled_back, 0);
        ASSERT_EQ(env_written, 1);
    }

    ASSERT_EQ(ab_uboot_boot_step(&env, &boot_slot, &env_written, &rolled_back, &wdt), 0);
    ASSERT_EQ(boot_slot, AB_SLOT_2);
    ASSERT_EQ(rolled_back, 1);
    ASSERT_EQ(env_written, 1);
    ASSERT_EQ(env.active_slot, AB_SLOT_2);
    ASSERT_EQ(env.last_good_slot, AB_SLOT_2);
    ASSERT_EQ(env.pending_slot, AB_SLOT_NONE);
    ASSERT_EQ(env.ab_state, AB_STATE_ROLLED_BACK);
    ASSERT_EQ(env.upgrade_available, 0);
    ASSERT_EQ(env.bootcount, 0UL);
    ASSERT_EQ(env.rollback_count, 1UL);
    ASSERT_STREQ(env.last_error, "bootlimit_exceeded");
    ASSERT_EQ(ab_env_validate(&env, err, sizeof(err)), 0);
    ASSERT_EQ(ab_stage_trial(&env, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)), 0);
}

static void test_watchdog_and_validation_invariants(void)
{
    ab_env_t env;
    char err[128] = {0};

    ASSERT_EQ(ab_watchdog_trial_expired(59999, 60000, 0), 0);
    ASSERT_EQ(ab_watchdog_trial_expired(60000, 60000, 0), 1);
    ASSERT_EQ(ab_watchdog_trial_expired(120000, 60000, 1), 0);

    ab_env_init_default(&env);
    env.bootlimit = 0;
    ASSERT_TRUE(ab_env_validate(&env, err, sizeof(err)) != 0);

    ab_env_init_default(&env);
    env.ab_state = AB_STATE_TRIAL;
    env.upgrade_available = 1;
    env.pending_slot = AB_SLOT_1;
    env.active_slot = AB_SLOT_1;
    env.last_good_slot = AB_SLOT_1;
    ASSERT_TRUE(ab_env_validate(&env, err, sizeof(err)) != 0);
}

int main(void)
{
    test_defaults_and_helpers();
    test_stage_guards_and_confirm_bidirectional();
    test_bootlimit_auto_rollback();
    test_watchdog_and_validation_invariants();

    printf("ab_state_test: PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
