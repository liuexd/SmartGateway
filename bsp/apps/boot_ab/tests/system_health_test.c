#include "system_health.h"
#include "ab_state.h"

#include <stdio.h>
#include <string.h>

static int checks;
static int failures;
#define CHECK(x) do { checks++; if (!(x)) { \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); failures++; } } while (0)

static sgw_health_sample_t healthy(void)
{
    sgw_health_sample_t s;
    memset(&s, 0, sizeof(s));
    snprintf(s.identity.boot_id, sizeof(s.identity.boot_id),
             "01234567-89ab-cdef-0123-456789abcdef");
    snprintf(s.identity.transaction_id, sizeof(s.identity.transaction_id),
             "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    s.identity.slot = 1;
    s.identity.bootcount = 1;
    s.system = s.supervisor = s.gateway = s.gpio = s.i2c = s.network = s.data =
        s.external = SGW_HEALTH_HEALTHY;
    return s;
}

static sgw_health_assessment_t observe(sgw_health_tracker_t *t,
                                       sgw_health_sample_t *s, uint64_t time)
{
    sgw_health_assessment_t out;
    s->time_ms = time;
    CHECK(sgw_health_tracker_sample(t, s, &out) == 0);
    return out;
}

int main(void)
{
    sgw_health_tracker_t t;
    sgw_health_policy_t p = {3000, 1000, 10000};
    sgw_health_sample_t s = healthy();
    sgw_health_assessment_t a;
    ab_env_t simulated;
    char err[128];
    unsigned int i;
    sgw_health_state_t *local[] = {&s.system, &s.supervisor, &s.gateway,
                                  &s.gpio, &s.i2c, &s.network, &s.data};

    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 100) == 0);
    a = observe(&t, &s, 100);
    CHECK(a.state == SGW_HEALTH_HEALTHY && !a.stable);
    for (i = 1; i <= 3; ++i) a = observe(&t, &s, 100 + 1000 * i);
    CHECK(a.state == SGW_HEALTH_HEALTHY && a.local_checks_passed && a.stable);
    /* In-memory model only: production confirmation remains disabled. */
    ab_env_init_default(&simulated);
    CHECK(ab_stage_trial(&simulated, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)) == 0);
    CHECK(ab_confirm_trial(&simulated, AB_SLOT_1,
                           a.local_checks_passed && a.stable, err, sizeof(err)) == 0);
    CHECK(simulated.ab_state == AB_STATE_CONFIRMED &&
          simulated.last_good_slot == AB_SLOT_1);

    /* Every mandatory subsystem independently blocks for all non-healthy states. */
    for (i = 0; i < 7; ++i) {
        sgw_health_state_t value;
        for (value = SGW_HEALTH_UNKNOWN; value <= SGW_HEALTH_UNHEALTHY; ++value) {
            if (value == SGW_HEALTH_HEALTHY) continue;
            CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) == 0);
            *local[i] = value;
            a = observe(&t, &s, 0);
            CHECK(!a.stable && !a.local_checks_passed);
            CHECK(a.state == value);
            ab_env_init_default(&simulated);
            CHECK(ab_stage_trial(&simulated, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)) == 0);
            CHECK(ab_confirm_trial(&simulated, AB_SLOT_1,
                                   a.local_checks_passed && a.stable,
                                   err, sizeof(err)) != 0);
            CHECK(simulated.last_good_slot == AB_SLOT_2);
            *local[i] = SGW_HEALTH_HEALTHY;
        }
    }

    /* An unavailable external service is degraded, not a local health failure. */
    s.external = SGW_HEALTH_UNHEALTHY;
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) == 0);
    for (i = 0; i <= 3; ++i) a = observe(&t, &s, i * 1000);
    CHECK(a.state == SGW_HEALTH_DEGRADED && a.local_checks_passed && a.stable);
    ab_env_init_default(&simulated);
    CHECK(ab_stage_trial(&simulated, AB_SLOT_1, AB_SLOT_2, err, sizeof(err)) == 0);
    CHECK(ab_confirm_trial(&simulated, AB_SLOT_1,
                           a.local_checks_passed && a.stable, err, sizeof(err)) == 0);
    s.external = SGW_HEALTH_UNKNOWN;
    a = observe(&t, &s, 4000);
    CHECK(a.state == SGW_HEALTH_DEGRADED && a.stable);
    s.external = SGW_HEALTH_HEALTHY;

    /* Interrupted/gapped observations restart the stable interval. */
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) == 0);
    a = observe(&t, &s, 0);
    a = observe(&t, &s, 1000);
    s.gateway = SGW_HEALTH_UNHEALTHY;
    a = observe(&t, &s, 2000);
    CHECK(a.state == SGW_HEALTH_UNHEALTHY && !a.stable);
    s.gateway = SGW_HEALTH_HEALTHY;
    a = observe(&t, &s, 3000);
    a = observe(&t, &s, 5000); /* gap exceeds policy */
    a = observe(&t, &s, 6000);
    CHECK(!a.stable);

    /* Identity changes and clock regressions invalidate evidence. */
    s.identity.slot = 2;
    s.time_ms = 7000;
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0 && !a.stable);
    s.identity.slot = 1;
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0); /* invalidation is sticky */
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 6000) == 0);
    s.time_ms = 100;
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0 && !a.stable);
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 6000) == 0);
    s.time_ms = 7000;
    s.identity.bootcount = 2;
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0 && !a.stable);
    s.identity.bootcount = 1;
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 6000) == 0);
    s.identity.transaction_id[0] = 'f';
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0 && !a.stable);
    s.identity.transaction_id[0] = '0';
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 6000) == 0);
    s.identity.boot_id[0] = 'f';
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0 && !a.stable);
    s.identity.boot_id[0] = '0';

    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) == 0);
    a = observe(&t, &s, 10001);
    CHECK(a.timed_out && !a.stable);
    s.identity.transaction_id[0] = '\0';
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) != 0);
    s = healthy();
    s.identity.bootcount = 0;
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) != 0);
    s = healthy();
    p.stable_ms = 0;
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) != 0);
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0);

    s = healthy();
    p.stable_ms = 3000;
    CHECK(sgw_health_tracker_init(&t, &s.identity, &p, 0) == 0);
    s.gpio = (sgw_health_state_t)99;
    CHECK(sgw_health_tracker_sample(&t, &s, &a) != 0 && !a.stable);

    printf("system_health_test: checks=%d failures=%d\n", checks, failures);
    return failures != 0;
}
