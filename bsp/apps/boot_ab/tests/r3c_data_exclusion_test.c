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
    char sysfs[128], disk[192], p3[256], p3link[256], polpath[256], err[256];
    const r3c_sector_range_t env_res[1] = { { 1792ULL, 32ULL } };
    const char *excl_paths[1] = { "/dev/mmcblk1p1" };
    struct stat dev_st;
    unsigned int maj, min;
    int dirfd;
    r3c_policy_t pol;

    assert(dir != NULL);
    assert(chmod(dir, 0700) == 0);
    /* Use real /dev/null? No, need S_ISBLK or direct r3c_data_check_slot_exclusion_sysfs */
    snprintf(sysfs, sizeof(sysfs), "%s/sysblock", dir);
    snprintf(disk, sizeof(disk), "%s/mmcblk1", dir);
    snprintf(p3, sizeof(p3), "%s/mmcblk1/mmcblk1p3", dir);
    assert(mkdir(sysfs, 0700) == 0);
    assert(mkdir(disk, 0700) == 0);
    assert(mkdir(p3, 0700) == 0);

    maj = 179; min = 3;
    snprintf(p3link, sizeof(p3link), "%s/%u:%u", sysfs, maj, min);
    assert(symlink(p3, p3link) == 0);
    {
        char f1[512], f2[512], f3[512];
        snprintf(f1, sizeof(f1), "%s/partition", p3);
        snprintf(f2, sizeof(f2), "%s/start", p3);
        snprintf(f3, sizeof(f3), "%s/size", p3);
        write_str(f1, "3\n");
        write_str(f2, "4096\n");
        write_str(f3, "1024000\n");
    }

    /* 1. Valid partition after reserved env range passes */
    assert(r3c_data_check_slot_exclusion_sysfs(
        maj, min, "/dev/mmcblk1p3", excl_paths, 1, env_res, 1, sysfs, err, sizeof(err)) == 0);

    /* 2. Direct excluded path match is rejected */
    assert(r3c_data_check_slot_exclusion_sysfs(
        maj, min, "/dev/mmcblk1p1", excl_paths, 1, env_res, 1, sysfs, err, sizeof(err)) != 0);

    /* 3. Overlap with U-Boot env raw sectors 1792..1824 is rejected */
    {
        char f2[512];
        snprintf(f2, sizeof(f2), "%s/start", p3);
        write_str(f2, "1800\n");
    }
    assert(r3c_data_check_slot_exclusion_sysfs(
        maj, min, "/dev/mmcblk1p3", excl_paths, 1, env_res, 1, sysfs, err, sizeof(err)) != 0);

    /* 4. Whole disk without partition entry is rejected */
    {
        char dlink[512], fsize[512];
        snprintf(dlink, sizeof(dlink), "%s/179:0", sysfs);
        assert(symlink(disk, dlink) == 0);
        snprintf(fsize, sizeof(fsize), "%s/size", disk);
        write_str(fsize, "7634944\n");
        assert(r3c_data_check_slot_exclusion_sysfs(
            179, 0, "/dev/mmcblk1", excl_paths, 1, env_res, 1, sysfs, err, sizeof(err)) != 0);
    }

    /* 5. Persistence probe on real directory */
    dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    assert(dirfd >= 0);
    assert(r3c_data_verify_persistence(dirfd, geteuid(), err, sizeof(err)) == 0);
    close(dirfd);

    /* 6. Policy loader: valid under home or non-writable parent? /tmp has 1777 so rejected! */
    snprintf(polpath, sizeof(polpath), "%s/policy.conf", dir);
    write_str(polpath, "gateway_flow_mode=controlled_local\nallow_external_degraded=1\n");
    assert(chmod(polpath, 0600) == 0);
    assert(r3c_policy_load(polpath, geteuid(), &pol, err, sizeof(err)) != 0); /* /tmp is 1777 */

    /* Test trusted policy path in HOME */
    {
        const char *home = getenv("HOME");
        char hdir[512], hp[1024];
        assert(home != NULL);
        snprintf(hdir, sizeof(hdir), "%s/.sgw-pol-test-%ld", home, (long)getpid());
        assert(mkdir(hdir, 0700) == 0);
        snprintf(hp, sizeof(hp), "%s/policy.conf", hdir);
        write_str(hp, "gateway_flow_mode=controlled_local\nallow_external_degraded=1\n");
        assert(chmod(hp, 0600) == 0);
        assert(r3c_policy_load(hp, geteuid(), &pol, err, sizeof(err)) == 0);
        assert(pol.gateway_flow_mode == R3C_GW_CONTROLLED_LOOPBACK);
        assert(pol.allow_external_degraded == 1);
        /* World-writable rejected */
        assert(chmod(hp, 0666) == 0);
        assert(r3c_policy_load(hp, geteuid(), &pol, err, sizeof(err)) != 0);
        unlink(hp);
        rmdir(hdir);
    }
    (void)dev_st;
    printf("r3c_data_exclusion_test: PASS\n");
    return 0;
}
