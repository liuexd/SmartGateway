#ifndef SGW_R3C_BUNDLE_H
#define SGW_R3C_BUNDLE_H
#include <sys/types.h>

/*
 * Verified-package record producer. This does not install an image, bind a
 * boot instance, set pending, or authorize confirmation. The caller must
 * authenticate the passed directory/key descriptors, enforce the global OTA
 * lock and perform image write/readback using the verified image itself.
 */
int r3c_bundle_record(int bundle_dirfd, int transaction_dirfd, int pubkey_fd,
                      uid_t owner, unsigned int target_slot, unsigned int last_good,
                      char transaction_id[65]);
#endif
