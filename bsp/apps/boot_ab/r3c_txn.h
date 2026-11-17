#ifndef SGW_R3C_TXN_H
#define SGW_R3C_TXN_H

#include "ab_health.h"
#include "ab_state.h"
#include "fw_env_backend.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

typedef enum {
    R3C_TXN_INVALID = 0,
    R3C_TXN_VERIFIED,
    R3C_TXN_WRITTEN,
    R3C_TXN_PENDING,
    R3C_TXN_CONFIRMING,
    R3C_TXN_CONFIRMED
} r3c_txn_state_t;

typedef struct {
    r3c_txn_state_t state;
    char id[65];
    char package_sha256[65];
    char image_sha256[65];
    unsigned int target_slot;
    unsigned int last_good_slot;
    uint64_t written_size;
    char boot_id[37];
    unsigned long bootcount;
    int consumed;
} r3c_txn_record_t;

const char *r3c_txn_state_str(r3c_txn_state_t s);

/* Load active unconsumed transaction under txdir lock. Rejects missing, ambiguous, corrupt, or consumed. */
int r3c_txn_load_active(int txdir, uid_t owner, r3c_txn_record_t *out,
                        char *err, size_t errlen);

/* Load any specific transaction record by id (used to inspect consumed/confirmed records in tests). */
int r3c_txn_load_by_id(int txdir, const char *id, uid_t owner,
                       r3c_txn_record_t *out, char *err, size_t errlen);

/* Compute SHA-256 of regular file or block image bytes from offset 0 up to expected_len (or full regular file). */
int r3c_txn_digest_fd(int fd, uint64_t expected_len, char hex[65], uint64_t *actual_len);

/* Write verified rootfs image fd into slotfd, fsync, read back & verify SHA-256, then commit WRITTEN. */
int r3c_txn_write_and_verify_slot(int txdir, const char *id, uid_t owner,
                                  int imagefd, int slotfd,
                                  char *err, size_t errlen);

/* Transition WRITTEN -> PENDING after ab_stage_trial + fw_env save & readback verification. */
int r3c_txn_stage_pending(int txdir, const char *id, uid_t owner,
                          const fw_env_backend_cfg_t *fw_cfg,
                          const ab_health_cfg_t *hcfg,
                          ab_slot_t running_slot,
                          char *err, size_t errlen);

/* Pre-confirm write checkpoint: PENDING/CONFIRMING -> CONFIRMING with boot_id/bootcount binding. */
int r3c_txn_begin_confirm(int txdir, const char *id, uid_t owner,
                          ab_slot_t running_slot, ab_slot_t last_good_slot,
                          const char *boot_id, unsigned long bootcount,
                          int slotfd,
                          char *err, size_t errlen);

/* Post-fw_env-readback commit: CONFIRMING -> CONFIRMED with consumed=1. */
int r3c_txn_finish_confirm(int txdir, const char *id, uid_t owner,
                           const char *boot_id, unsigned long bootcount,
                           char *err, size_t errlen);

/* Reconcile interrupted CONFIRMING -> CONFIRMED(consumed=1) after fw_env already committed. */
int r3c_txn_reconcile_confirm(int txdir, const char *id, uid_t owner,
                              ab_slot_t running_slot, int slotfd,
                              char *err, size_t errlen);

/* Helper to acquire exclusive lock on <txdir>/.sgw-transaction.lock with strict checks. */
int r3c_txn_acquire_lock(int txdir, uid_t owner, int *lockfd, char *err, size_t errlen);

#endif
