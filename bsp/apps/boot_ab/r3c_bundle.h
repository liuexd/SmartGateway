#ifndef SGW_R3C_BUNDLE_H
#define SGW_R3C_BUNDLE_H
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef struct {
    uint32_t boot_cap;
    uint32_t min_sys_version;
    uint32_t data_schema;
    int require_compat_manifest;
} r3c_bundle_compat_t;

/*
 * Verified-package record producer. This does not install an image, bind a
 * boot instance, set pending, or authorize confirmation. The caller must
 * authenticate the passed directory/key descriptors, enforce the global OTA
 * lock and perform image write/readback using the verified image itself.
 */
int r3c_bundle_record(int bundle_dirfd, int transaction_dirfd, int pubkey_fd,
                      uid_t owner, unsigned int target_slot, unsigned int last_good,
                      char transaction_id[65]);

int r3c_bundle_record_checked(int bundle_dirfd, int transaction_dirfd, int pubkey_fd,
                              uid_t owner, unsigned int target_slot, unsigned int last_good,
                              const r3c_bundle_compat_t *compat,
                              char transaction_id[65], char *err, size_t errlen);
#endif
