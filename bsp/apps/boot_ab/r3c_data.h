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

typedef struct {
    uint64_t start_sector;
    uint64_t sector_count;
} r3c_sector_range_t;

typedef struct {
    const char *mountpoint;
    const char *device;
    const char *whole_disk_device;  /* e.g. /dev/mmcblk1 (checked separately from partitions) */
    const char *mountinfo_path;     /* NULL -> /proc/self/mountinfo */
    const char *sysfs_block_root;   /* NULL -> /sys/dev/block */
    const char *expected_fstype;    /* NULL -> "ext4" */
    const char *const *excluded_devices; /* system slot partitions ONLY (p1, p2) */
    size_t excluded_count;
    const r3c_sector_range_t *reserved_sectors;
    size_t reserved_count;
    uint64_t minimum_available_bytes;
    uid_t owner;
    int verify_write_persistence;
} r3c_data_full_policy_t;

int r3c_data_mountinfo_matches(const char *text, const char *mountpoint,
                               unsigned int dev_major, unsigned int dev_minor);

int r3c_data_mountinfo_matches_fstype(const char *text, const char *mountpoint,
                                      unsigned int dev_major, unsigned int dev_minor,
                                      const char *expected_fstype);

/* Fail-closed exclusion check: requires whole_disk_device & all excluded slot partitions to resolve cleanly. */
int r3c_data_check_slot_exclusion_sysfs(unsigned int dev_major, unsigned int dev_minor,
                                        const char *data_device,
                                        const char *whole_disk_device,
                                        const char *const *excluded_devices,
                                        size_t excluded_count,
                                        const r3c_sector_range_t *reserved_sectors,
                                        size_t reserved_count,
                                        const char *sysfs_block_root,
                                        uid_t owner,
                                        char *reason, size_t reason_size);

int r3c_data_verify_persistence(int dirfd, uid_t owner, char *reason, size_t reason_size);

int r3c_data_inspect(const r3c_data_policy_t *policy, char *reason, size_t reason_size);

int r3c_data_inspect_full(const r3c_data_full_policy_t *policy, char *reason, size_t reason_size);

int r3c_data_inspect_contents(int dirfd, uid_t owner, uint64_t minimum_available_bytes,
                              char *reason, size_t reason_size);

int r3c_app_ota_inspect(const char *install_dir, uid_t owner,
                        char *reason, size_t reason_size);

int r3c_app_ota_inspect_locked(const char *install_dir, uid_t owner, int *lockfd,
                               char *reason, size_t reason_size);

#endif
