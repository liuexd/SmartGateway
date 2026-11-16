#ifndef SGW_SYSTEM_HEALTH_H
#define SGW_SYSTEM_HEALTH_H

#include <stdint.h>

/* Pure assessment logic. No value from this API authorizes a real update. */
typedef enum {
    SGW_HEALTH_UNKNOWN = 0,
    SGW_HEALTH_HEALTHY,
    SGW_HEALTH_DEGRADED,
    SGW_HEALTH_UNHEALTHY
} sgw_health_state_t;

#define SGW_HEALTH_BOOT_ID_SIZE 37
#define SGW_HEALTH_TX_ID_SIZE 65

typedef struct {
    char boot_id[SGW_HEALTH_BOOT_ID_SIZE];
    char transaction_id[SGW_HEALTH_TX_ID_SIZE];
    unsigned int slot;
    unsigned long bootcount;
} sgw_health_identity_t;

typedef struct {
    sgw_health_identity_t identity;
    uint64_t time_ms;
    sgw_health_state_t system;
    sgw_health_state_t supervisor;
    sgw_health_state_t gateway;
    sgw_health_state_t gpio;
    sgw_health_state_t i2c;
    sgw_health_state_t network;
    sgw_health_state_t data;
    sgw_health_state_t external;
} sgw_health_sample_t;

typedef struct {
    uint64_t stable_ms;
    uint64_t max_gap_ms;
    uint64_t deadline_ms;
} sgw_health_policy_t;

typedef struct {
    sgw_health_identity_t identity;
    sgw_health_policy_t policy;
    uint64_t start_ms;
    uint64_t first_good_ms;
    uint64_t previous_ms;
    unsigned int samples;
    int have_good;
    int initialized;
} sgw_health_tracker_t;

typedef struct {
    sgw_health_state_t state;
    int local_checks_passed;
    int stable;
    int timed_out;
} sgw_health_assessment_t;

const char *sgw_health_state_name(sgw_health_state_t state);
int sgw_health_tracker_init(sgw_health_tracker_t *tracker,
                            const sgw_health_identity_t *identity,
                            const sgw_health_policy_t *policy,
                            uint64_t now_ms);
int sgw_health_tracker_sample(sgw_health_tracker_t *tracker,
                              const sgw_health_sample_t *sample,
                              sgw_health_assessment_t *out);

#endif
