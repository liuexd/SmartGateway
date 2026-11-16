#define _POSIX_C_SOURCE 200809L
#include "ab_health.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures;
#define CHECK(expr) do { if (!(expr)) { \
    fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr); failures++; \
} } while (0)

static void put(const char *path, const char *text)
{
    FILE *fp = fopen(path, "w");
    if (!fp) { perror(path); exit(1); }
    fputs(text, fp);
    fclose(fp);
}

int main(void)
{
    char dir[] = "/tmp/sgw-ab-safety-XXXXXX";
    char cmdline[256], mounts[256], err[256] = {0};
    ab_health_cfg_t cfg;
    ab_slot_t slot = AB_SLOT_NONE;
    ab_env_t env;

    if (!mkdtemp(dir)) return 1;
    snprintf(cmdline, sizeof(cmdline), "%s/cmdline", dir);
    snprintf(mounts, sizeof(mounts), "%s/mounts", dir);
    ab_health_cfg_init(&cfg);
    cfg.proc_cmdline_path = cmdline;
    cfg.proc_mounts_path = mounts;

    /* p2 root+device+mount agree; each mismatch is rejected. */
    CHECK(ab_root_identity_matches(1, 179, 10, 179, 10, 179, 10));
    CHECK(!ab_root_identity_matches(1, 0, 42, 179, 10, 0, 42)); /* tmpfs */
    CHECK(!ab_root_identity_matches(1, 179, 9, 179, 10, 179, 9)); /* p1 vs p2 */
    CHECK(!ab_root_identity_matches(1, 179, 10, 179, 10, 179, 9)); /* stale mountinfo */
    CHECK(!ab_root_identity_matches(0, 179, 10, 179, 10, 179, 10)); /* not block */

    put(cmdline, "root=/dev/mmcblk1p2 rw\n");
    put(mounts, "tmpfs / tmpfs rw 0 0\n");
    CHECK(ab_detect_running_slot(&cfg, &slot, err, sizeof(err)) != 0);
    CHECK(slot == AB_SLOT_NONE);

    put(mounts, "/dev/mmcblk1p1 / ext4 rw 0 0\n");
    CHECK(ab_detect_running_slot(&cfg, &slot, err, sizeof(err)) != 0);
    CHECK(strstr(err, "mismatch") != NULL);

    put(cmdline, "root=PARTUUID=unknown rw\n");
    put(mounts, "/dev/root / ext4 rw 0 0\n");
    CHECK(ab_detect_running_slot(&cfg, &slot, err, sizeof(err)) != 0);

    ab_env_init_default(&env);
    CHECK(ab_verify_system_health(&cfg, &env, &slot, NULL, err, sizeof(err)) != 0);

    unlink(cmdline);
    unlink(mounts);
    rmdir(dir);
    printf("ab_health_test: FAIL=%d; unverified roots and missing health rejected\n", failures);
    return failures != 0;
}
