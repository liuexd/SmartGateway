#include "system_health.h"

#include <ctype.h>
#include <stddef.h>
#include <string.h>

static int state_valid(sgw_health_state_t state)
{
    return state >= SGW_HEALTH_UNKNOWN && state <= SGW_HEALTH_UNHEALTHY;
}

const char *sgw_health_state_name(sgw_health_state_t state)
{
    switch (state) {
    case SGW_HEALTH_HEALTHY: return "HEALTHY";
    case SGW_HEALTH_DEGRADED: return "DEGRADED";
    case SGW_HEALTH_UNHEALTHY: return "UNHEALTHY";
    default: return "UNKNOWN";
    }
}

static int identity_valid(const sgw_health_identity_t *id)
{
    unsigned int i;
    int nonzero = 0;

    if (!id || (id->slot != 1 && id->slot != 2) || id->bootcount == 0 ||
        strnlen(id->boot_id, sizeof(id->boot_id)) != 36 ||
        strnlen(id->transaction_id, sizeof(id->transaction_id)) != 64)
        return 0;
    for (i = 0; i < 36; ++i) {
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (id->boot_id[i] != '-') return 0;
        } else if (!isxdigit((unsigned char)id->boot_id[i])) {
            return 0;
        }
    }
    for (i = 0; i < 64; ++i) {
        if (!isxdigit((unsigned char)id->transaction_id[i])) return 0;
        if (id->transaction_id[i] != '0') nonzero = 1;
    }
    return nonzero;
}

static int identity_equal(const sgw_health_identity_t *a,
                          const sgw_health_identity_t *b)
{
    return a->slot == b->slot && a->bootcount == b->bootcount &&
           strcmp(a->boot_id, b->boot_id) == 0 &&
           strcmp(a->transaction_id, b->transaction_id) == 0;
}

int sgw_health_tracker_init(sgw_health_tracker_t *tracker,
                            const sgw_health_identity_t *identity,
                            const sgw_health_policy_t *policy,
                            uint64_t now_ms)
{
    if (!tracker) return -1;
    memset(tracker, 0, sizeof(*tracker));
    if (!identity || !policy || !identity_valid(identity) ||
        policy->stable_ms == 0 || policy->max_gap_ms == 0 ||
        policy->deadline_ms < policy->stable_ms)
        return -1;
    tracker->identity = *identity;
    tracker->policy = *policy;
    tracker->start_ms = now_ms;
    tracker->previous_ms = now_ms;
    tracker->initialized = 1;
    return 0;
}

int sgw_health_tracker_sample(sgw_health_tracker_t *tracker,
                              const sgw_health_sample_t *sample,
                              sgw_health_assessment_t *out)
{
    sgw_health_state_t local[7];
    unsigned int i;
    int failed = 0, unknown = 0, degraded = 0;

    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    out->state = SGW_HEALTH_UNKNOWN;
    if (!tracker || !sample || !tracker->initialized) return -1;

    if (!identity_valid(&sample->identity) ||
        !identity_equal(&tracker->identity, &sample->identity) ||
        sample->time_ms < tracker->previous_ms ||
        sample->time_ms < tracker->start_ms) {
        tracker->have_good = 0;
        tracker->samples = 0;
        tracker->initialized = 0;
        return -1;
    }

    local[0] = sample->system;
    local[1] = sample->supervisor;
    local[2] = sample->gateway;
    local[3] = sample->gpio;
    local[4] = sample->i2c;
    local[5] = sample->network;
    local[6] = sample->data;
    for (i = 0; i < 7; ++i) {
        if (!state_valid(local[i])) {
            tracker->have_good = 0;
            tracker->samples = 0;
            tracker->initialized = 0;
            return -1;
        }
        if (local[i] == SGW_HEALTH_UNHEALTHY) failed = 1;
        if (local[i] == SGW_HEALTH_UNKNOWN) unknown = 1;
        if (local[i] == SGW_HEALTH_DEGRADED) degraded = 1;
    }
    if (!state_valid(sample->external)) {
        tracker->have_good = 0;
        tracker->samples = 0;
        tracker->initialized = 0;
        return -1;
    }

    if (sample->time_ms - tracker->start_ms > tracker->policy.deadline_ms) {
        tracker->have_good = 0;
        tracker->samples = 0;
        tracker->previous_ms = sample->time_ms;
        out->timed_out = 1;
        return 0;
    }

    out->local_checks_passed = !failed && !unknown && !degraded;
    if (failed) out->state = SGW_HEALTH_UNHEALTHY;
    else if (unknown) out->state = SGW_HEALTH_UNKNOWN;
    else if (degraded || sample->external != SGW_HEALTH_HEALTHY)
        out->state = SGW_HEALTH_DEGRADED;
    else out->state = SGW_HEALTH_HEALTHY;

    if (!out->local_checks_passed) {
        tracker->have_good = 0;
        tracker->samples = 0;
    } else {
        if (!tracker->have_good ||
            sample->time_ms - tracker->previous_ms > tracker->policy.max_gap_ms) {
            tracker->first_good_ms = sample->time_ms;
            tracker->samples = 1;
            tracker->have_good = 1;
        } else {
            if (tracker->samples < 2) tracker->samples++;
        }
        if (tracker->samples >= 2 &&
            sample->time_ms - tracker->first_good_ms >= tracker->policy.stable_ms)
            out->stable = 1;
    }
    tracker->previous_ms = sample->time_ms;
    return 0;
}
