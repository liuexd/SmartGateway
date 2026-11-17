#ifndef SGW_R3C_BINDING_H
#define SGW_R3C_BINDING_H

#include <stdint.h>

/*
 * A comparison primitive only. The caller must authenticate each input,
 * obtain digests from the actual verified package/images, hold the upgrade
 * lock, check evidence freshness again and persist atomically. Passing this
 * function alone MUST NOT authorize confirmation.
 */
typedef struct {
    char transaction_id[65];
    char package_sha256[65];
    char image_sha256[65];
    char boot_id[37];
    unsigned int target_slot;
    unsigned int last_good_slot;
    unsigned long bootcount;
} r3c_binding_t;

int r3c_binding_matches(const r3c_binding_t *persisted,
                        const r3c_binding_t *observed,
                        uint64_t observed_ms, uint64_t now_ms,
                        uint64_t max_age_ms);

#endif
