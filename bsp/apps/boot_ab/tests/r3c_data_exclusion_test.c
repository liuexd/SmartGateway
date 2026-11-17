#define _GNU_SOURCE
#include "r3c_data.h"
#include "r3c_policy.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static void write_str(const char *path, const char *s)
{
    FILE *fp = fopen(path, "w");
    assert(fp != NULL);
    assert(fputs(s, fp) >= 0);
    assert(fclose(fp) == 0);
}

int main(void)
{
    char tmpl[] = "/tmp/.sgw-r3c-excl-XXXXXX";
    char *dir = mkdtemp(tmpl);
    char sysfs[128], disk[192], p3[256], p3link[256], err[256];
    const r3c_sector_range_t env_res[1] = { { 1792ULL, 32ULL } };
    struct stat st_sda, st_sda1, st_sda2;
    int dirfd;

    assert(dir != NULL);
    assert(chmod(dir, 0700) == 0);

    /* A. Real kernel /dev/sda* + /sys/dev/block tests */
    if (stat("/dev/sda", &st_sda) == 0 && S_ISBLK(st_sda.st_mode) &&
        stat("/dev/sda1", &st_sda1) == 0 && S_ISBLK(st_sda1.st_mode) &&
        stat("/dev/sda2", &st_sda2) == 0 && S_ISBLK(st_sda2.st_mode)) {
        const char *excl_sda1[1] = { "/dev/sda1" };
        const char *excl_missing[2] = { "/dev/sda1", "/dev/nonexistent_slot_xyz" };
        const char *excl_sda2[1] = { "/dev/sda2" };

        /* 1. Valid distinct partition on same physical disk passes against whole disk + sda1 */
        assert(r3c_data_check_slot_exclusion_sysfs(
            major(st_sda2.st_rdev), minor(st_sda2.st_rdev),
            "/dev/sda2", "/dev/sda", excl_sda1, 1, env_res, 1,
            "/sys/dev/block", 0, err, sizeof(err)) == 0);

        /* 2. Unknown/missing excluded slot MUST FAIL CLOSED */
        assert(r3c_data_check_slot_exclusion_sysfs(
            major(st_sda2.st_rdev), minor(st_sda2.st_rdev),
            "/dev/sda2", "/dev/sda", excl_missing, 2, env_res, 1,
            "/sys/dev/block", 0, err, sizeof(err)) != 0);

        /* 3. Unknown whole disk MUST FAIL CLOSED */
        assert(r3c_data_check_slot_exclusion_sysfs(
            major(st_sda2.st_rdev), minor(st_sda2.st_rdev),
            "/dev/sda2", "/dev/missing_disk", excl_sda1, 1, env_res, 1,
            "/sys/dev/block", 0, err, sizeof(err)) != 0);

        /* 4. Whole disk passed as DATA candidate MUST FAIL CLOSED */
        assert(r3c_data_check_slot_exclusion_sysfs(
            major(st_sda.st_rdev), minor(st_sda.st_rdev),
            "/dev/sda", "/dev/sda", excl_sda1, 1, env_res, 1,
            "/sys/dev/block", 0, err, sizeof(err)) != 0);

        /* 5. Same partition as excluded slot MUST FAIL CLOSED */
        assert(r3c_data_check_slot_exclusion_sysfs(
            major(st_sda2.st_rdev), minor(st_sda2.st_rdev),
            "/dev/sda2", "/dev/sda", excl_sda2, 1, env_res, 1,
            "/sys/dev/block", 0, err, sizeof(err)) != 0);

        /* 6. r3c_data_inspect_full rejects /tmp untrusted path AND rejects symlink to real /dev/sda2 */
        {
            const char *home = getenv("HOME");
            char hdir[256], sym[512], mp[512];
            r3c_data_full_policy_t fp;
            assert(home != NULL);
            snprintf(hdir, sizeof(hdir), "%s/.sgw-full-test-%ld", home, (long)getpid());
            assert(mkdir(hdir, 0700) == 0);
            snprintf(sym, sizeof(sym), "%s/sym_sda2", hdir);
            snprintf(mp, sizeof(mp), "%s/mnt", hdir);
            assert(mkdir(mp, 0700) == 0);
            assert(symlink("/dev/sda2", sym) == 0);

            memset(&fp, 0, sizeof(fp));
            fp.mountpoint = mp;
            fp.device = sym; /* Symlink device MUST be rejected by trusted_block_node */
            fp.whole_disk_device = "/dev/sda";
            fp.excluded_devices = excl_sda1;
            fp.excluded_count = 1;
            fp.reserved_sectors = env_res;
            fp.reserved_count = 1;
            fp.minimum_available_bytes = 1024;
            fp.owner = geteuid();
            assert(r3c_data_inspect_full(&fp, err, sizeof(err)) != 0);
            assert(strstr(err, "untrusted") != NULL);

            /* Real /dev/sda2 passes trusted_block_node check and is rejected only because mp isn't /dev/sda2 mount */
            fp.device = "/dev/sda2";
            assert(r3c_data_inspect_full(&fp, err, sizeof(err)) != 0);
            assert(strstr(err, "does not match") != NULL);

            unlink(sym);
            rmdir(mp);
            rmdir(hdir);
        }
    }

    /* B. Isolated sysfs reserved-sector overlap check (1792..1824) */
    snprintf(sysfs, sizeof(sysfs), "%s/sysblock", dir);
    snprintf(disk, sizeof(disk), "%s/mmcblk1", dir);
    snprintf(p3, sizeof(p3), "%s/mmcblk1/mmcblk1p3", dir);
    assert(mkdir(sysfs, 0700) == 0);
    assert(mkdir(disk, 0700) == 0);
    assert(mkdir(p3, 0700) == 0);
    snprintf(p3link, sizeof(p3link), "%s/179:3", sysfs);
    assert(symlink(p3, p3link) == 0);
    {
        char f1[512], f2[512], f3[512];
        snprintf(f1, sizeof(f1), "%s/partition", p3);
        snprintf(f2, sizeof(f2), "%s/start", p3);
        snprintf(f3, sizeof(f3), "%s/size", p3);
        write_str(f1, "3\n");
        write_str(f2, "1800\n");
        write_str(f3, "1024000\n");
    }
    /* Missing excluded device fails closed here too */
    {
        const char *excl[1] = { "/dev/mmcblk1p1" };
        assert(r3c_data_check_slot_exclusion_sysfs(
            179, 3, "/dev/mmcblk1p3", "/dev/mmcblk1", excl, 1, env_res, 1,
            sysfs, geteuid(), err, sizeof(err)) != 0);
    }

    /* C. Persistence probe + Policy bounds */
    dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dirfd >= 0);
    assert(r3c_data_verify_persistence(dirfd, geteuid(), err, sizeof(err)) == 0);
    close(dirfd);

    {
        const char *home = getenv("HOME");
        char hdir[256], hp[512];
        r3c_policy_t pol;
        assert(home != NULL);
        snprintf(hdir, sizeof(hdir), "%s/.sgw-pol-test-%ld", home, (long)getpid());
        assert(mkdir(hdir, 0700) == 0);
        snprintf(hp, sizeof(hp), "%s/policy.conf", hdir);
        write_str(hp, "max_evidence_age_ms=6000\n");
        assert(chmod(hp, 0600) == 0);
        assert(r3c_policy_load(hp, geteuid(), &pol, err, sizeof(err)) != 0);
        write_str(hp, "data_mountpoint=/\n");
        assert(r3c_policy_load(hp, geteuid(), &pol, err, sizeof(err)) != 0);
        write_str(hp, "data_device=/dev/mmcblk1p1\n");
        assert(r3c_policy_load(hp, geteuid(), &pol, err, sizeof(err)) != 0);
        write_str(hp, "gateway_flow_mode=controlled_local\nallow_external_degraded=1\n");
        assert(r3c_policy_load(hp, geteuid(), &pol, err, sizeof(err)) == 0);
        assert(pol.gateway_flow_mode == R3C_GW_CONTROLLED_LOOPBACK);
        assert(pol.allow_external_degraded == 1);
        assert(chmod(hp, 0666) == 0);
        assert(r3c_policy_load(hp, geteuid(), &pol, err, sizeof(err)) != 0);
        unlink(hp);
        rmdir(hdir);
    }

    printf("r3c_data_exclusion_test: PASS\n");
    return 0;
}
