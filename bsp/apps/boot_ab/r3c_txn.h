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
    R3C_TXN_CONFIRMED,
    R3C_TXN_ROLLED_BACK
} r3c_txn_state_t;

typedef struct {
    r3c_txn_state_t state;
    char id[65];
    char package_sha256[65];
    char image_sha256[65];
    char rootfs_manifest_sha256[65]; /* empty if SGW-SYSTEM-1 */
    unsigned int target_slot;
    unsigned int last_good_slot;
    uint64_t written_size;
    char boot_id[37];
    unsigned long bootcount;
    int consumed;
} r3c_txn_record_t;

typedef struct {
    const char *target_dev_override;  /* NULL in prod -> ab_slot_device(rec.target_slot) */
    const char *mountinfo_path;       /* NULL in prod -> "/proc/self/mountinfo" */
    const char *sysfs_block_root;     /* NULL in prod -> "/sys/dev/block" */
    int allow_regular_file_target;    /* 0 in prod (requires S_ISBLK), 1 ONLY in harness */
    int allow_host_slot_sim;          /* 0 in prod, 1 ONLY in harness */
} r3c_slot_write_opts_t;

const char *r3c_txn_state_str(r3c_txn_state_t s);

int r3c_txn_load_active(int txdir, uid_t owner, r3c_txn_record_t *out,
                        char *err, size_t errlen);

int r3c_txn_load_by_id(int txdir, const char *id, uid_t owner,
                       r3c_txn_record_t *out, char *err, size_t errlen);

int r3c_txn_digest_fd(int fd, uint64_t expected_len, char hex[65], uint64_t *actual_len);

/* Self-validating slot writer: verifies running/last_good/mountinfo/capacity before writing 1 byte. */
int r3c_txn_write_and_verify_slot(int txdir, const char *id, uid_t owner,
                                  int imagefd,
                                  const fw_env_backend_cfg_t *fw_cfg,
                                  const ab_health_cfg_t *hcfg,
                                  const r3c_slot_write_opts_t *opts,
                                  char *err, size_t errlen);

/* Verify runtime sysroot identity: signed rootfs.manifest for rw rootfs, or raw block for ro rootfs. */
int r3c_txn_verify_installed_rootfs(int txdir, const r3c_txn_record_t *rec, uid_t owner,
                                    const char *sysroot_prefix, int root_is_rw,
                                    int raw_slotfd,
                                    char *err, size_t errlen);

int r3c_txn_stage_pending(int txdir, const char *id, uid_t owner,
                          const fw_env_backend_cfg_t *fw_cfg,
                          const ab_health_cfg_t *hcfg,
                          ab_slot_t running_slot,
                          char *err, size_t errlen);

int r3c_txn_begin_confirm(int txdir, const char *id, uid_t owner,
                          ab_slot_t running_slot, ab_slot_t last_good_slot,
                          const char *boot_id, unsigned long bootcount,
                          const char *sysroot_prefix, int root_is_rw, int slotfd,
                          char *err, size_t errlen);

int r3c_txn_finish_confirm(int txdir, const char *id, uid_t owner,
                           const char *boot_id, unsigned long bootcount,
                           char *err, size_t errlen);

int r3c_txn_reconcile_confirm(int txdir, const char *id, uid_t owner,
                              ab_slot_t running_slot,
                              const char *sysroot_prefix, int root_is_rw, int slotfd,
                              char *err, size_t errlen);

int r3c_txn_acquire_lock(int txdir, uid_t owner, int *lockfd, char *err, size_t errlen);

int r3c_txn_reconcile_rollback(int txdir, uid_t owner,
                               const ab_env_t *env, ab_slot_t running_slot,
                               const char *boot_id, int *out_reconciled,
                               char out_txid[65], char *err, size_t errlen);

#endif
