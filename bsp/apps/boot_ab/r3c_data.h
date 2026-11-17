#ifndef SGW_R3C_DATA_H
#define SGW_R3C_DATA_H

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

/*
 * Read-only prerequisites, NOT an authorization or confirmation decision.
 * Caller must obtain policy from a protected, separately reviewed source.
 */
typedef struct {
    const char *mountpoint;
    const char *device;
    uint64_t minimum_available_bytes;
    uid_t owner;
} r3c_data_policy_t;

/* Parse a mountinfo snapshot. Reject missing, ambiguous, bind/subdir or ro mounts. */
int r3c_data_mountinfo_matches(const char *text, const char *mountpoint,
                               unsigned int dev_major, unsigned int dev_minor);

/* Verify the live mount, block identity, capacity, permissions and schema=1. */
int r3c_data_inspect(const r3c_data_policy_t *policy, char *reason, size_t reason_size);

/* Contents check only; caller must separately authenticate the mount/device. */
int r3c_data_inspect_contents(int dirfd, uid_t owner, uint64_t minimum_available_bytes,
                              char *reason, size_t reason_size);

/* Inspect existing application OTA state under a pre-existing shared lock. */
int r3c_app_ota_inspect(const char *install_dir, uid_t owner,
                        char *reason, size_t reason_size);

/* On success the caller owns *lockfd and must hold it until its operation ends. */
int r3c_app_ota_inspect_locked(const char *install_dir, uid_t owner, int *lockfd,
                               char *reason, size_t reason_size);

#endif
