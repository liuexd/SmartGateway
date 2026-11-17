#define _POSIX_C_SOURCE 200809L
#include "ab_health.h"
#include "service_ipc.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

static void set_err(char *errbuf, size_t errlen, const char *msg)
{
    if (errbuf && errlen > 0) {
        snprintf(errbuf, errlen, "%s", msg ? msg : "unknown error");
    }
}

void ab_health_cfg_init(ab_health_cfg_t *cfg)
{
    const char *v;

    if (!cfg)
        return;

    v = getenv("SGW_PROC_CMDLINE");
    cfg->proc_cmdline_path = (v && v[0]) ? v : "/proc/cmdline";

    v = getenv("SGW_PROC_MOUNTS");
    cfg->proc_mounts_path = (v && v[0]) ? v : "/proc/mounts";

    v = getenv("SGW_SUPERVISOR_RUN_DIR");
    cfg->supervisor_run_dir = (v && v[0]) ? v : "/opt/smartgateway/run";

    v = getenv("SGW_SYSROOT_PREFIX");
    cfg->sysroot_prefix = (v && v[0]) ? v : "";

    cfg->supervisor_timeout_ms = 2000;
    cfg->verify_supervisor_pid = 1;
}

static int parse_slot_from_dev(const char *dev, ab_slot_t *out_slot)
{
    if (!dev || !out_slot)
        return -1;
    if (strcmp(dev, "/dev/mmcblk1p1") == 0) {
        *out_slot = AB_SLOT_1;
        return 0;
    }
    if (strcmp(dev, "/dev/mmcblk1p2") == 0) {
        *out_slot = AB_SLOT_2;
        return 0;
    }
    return -1;
}

int ab_root_identity_matches(int slot_is_block,
                             unsigned int root_major, unsigned int root_minor,
                             unsigned int slot_major, unsigned int slot_minor,
                             unsigned int mount_major, unsigned int mount_minor)
{
    return slot_is_block && root_major == slot_major && root_minor == slot_minor &&
           root_major == mount_major && root_minor == mount_minor;
}

static int read_first_line(const char *path, char *buf, size_t cap)
{
    FILE *fp = fopen(path, "r");
    size_t n;

    if (!fp)
        return -1;
    if (!fgets(buf, (int)cap, fp)) {
        fclose(fp);
        return -1;
    }
    fclose(fp);
    n = strlen(buf);
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
        buf[--n] = '\0';
    return 0;
}

static int find_root_mount_info(const char *mounts_path,
                                char *dev_out,
                                size_t dev_cap,
                                int *out_is_rw,
                                char *errbuf,
                                size_t errlen)
{
    FILE *fp = fopen(mounts_path, "r");
    char line[512];
    int found = 0;

    if (!fp) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "cannot open mounts file '%s': %s", mounts_path, strerror(errno));
        return -1;
    }

    while (fgets(line, sizeof(line), fp)) {
        char dev[128] = {0};
        char mnt[128] = {0};
        char fstype[64] = {0};
        char opts[256] = {0};

        if (sscanf(line, "%127s %127s %63s %255s", dev, mnt, fstype, opts) >= 4) {
            if (strcmp(mnt, "/") == 0) {
                snprintf(dev_out, dev_cap, "%s", dev);
                if (out_is_rw) {
                    int is_rw = 0;
                    char *tok = opts;
                    char *comma;
                    while (tok && *tok) {
                        comma = strchr(tok, ',');
                        if (comma) *comma = '\0';
                        if (strcmp(tok, "rw") == 0)
                            is_rw = 1;
                        if (strcmp(tok, "ro") == 0)
                            is_rw = 0;
                        tok = comma ? (comma + 1) : NULL;
                    }
                    *out_is_rw = is_rw;
                }
                found = 1;
            }
        }
    }
    fclose(fp);

    if (!found) {
        set_err(errbuf, errlen, "no root '/' entry found in mounts");
        return -1;
    }
    return 0;
}

int ab_detect_running_slot_full(const ab_health_cfg_t *cfg,
                                const char *mountinfo_path,
                                int skip_blk_identity,
                                ab_slot_t *out_slot,
                                char *errbuf,
                                size_t errlen)
{
    ab_health_cfg_t effective;
    char cmdline[1024];
    char *tok;
    char *saveptr = NULL;
    ab_slot_t cmdline_slot = AB_SLOT_NONE;
    char mount_dev[128] = {0};
    ab_slot_t mount_slot = AB_SLOT_NONE;
    int is_rw = 0;

    if (!out_slot) {
        set_err(errbuf, errlen, "null out_slot");
        return -1;
    }
    *out_slot = AB_SLOT_NONE;

    if (cfg) {
        effective = *cfg;
    } else {
        ab_health_cfg_init(&effective);
    }

    if (read_first_line(effective.proc_cmdline_path, cmdline, sizeof(cmdline)) != 0) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "cannot read kernel cmdline '%s'", effective.proc_cmdline_path);
        return -1;
    }

    tok = strtok_r(cmdline, " \t", &saveptr);
    while (tok) {
        if (strncmp(tok, "root=", 5) == 0) {
            ab_slot_t parsed = AB_SLOT_NONE;
            if (parse_slot_from_dev(tok + 5, &parsed) == 0)
                cmdline_slot = parsed;
        }
        tok = strtok_r(NULL, " \t", &saveptr);
    }

    if (cmdline_slot != AB_SLOT_1 && cmdline_slot != AB_SLOT_2) {
        set_err(errbuf, errlen,
                "cannot determine running eMMC slot (/dev/mmcblk1p1|p2) from kernel cmdline");
        return -1;
    }

    if (find_root_mount_info(effective.proc_mounts_path, mount_dev,
                             sizeof(mount_dev), &is_rw, errbuf, errlen) != 0) {
        return -1;
    }

    if (parse_slot_from_dev(mount_dev, &mount_slot) == 0 && mount_slot != cmdline_slot) {
        set_err(errbuf, errlen,
                "split-brain mismatch between /proc/cmdline root and /proc/mounts /");
        return -1;
    }

    if (!skip_blk_identity) {
    /*
     * cmdline describes boot intent.  The VFS root device and mountinfo are
     * authoritative for what is actually mounted.  Reject overlays, tmpfs,
     * device-mapper, missing nodes, and any other unproven mapping.
     */
    {
        struct stat root_st, slot_st;
        FILE *fp;
        char line[4096];
        unsigned int ma, mi;
        char mountpoint[1024];
        int found = 0;
        const char *slot_path = ab_slot_device(cmdline_slot);

        if (!slot_path || stat("/", &root_st) != 0 ||
            stat(slot_path, &slot_st) != 0 || !S_ISBLK(slot_st.st_mode)) {
            set_err(errbuf, errlen, "actual root device does not match requested slot");
            return -1;
        }
        fp = fopen((mountinfo_path && mountinfo_path[0]) ? mountinfo_path : "/proc/self/mountinfo", "r");
        if (!fp) {
            set_err(errbuf, errlen, "cannot read root mount identity");
            return -1;
        }
        while (fgets(line, sizeof(line), fp)) {
            if (sscanf(line, "%*u %*u %u:%u %*s %1023s", &ma, &mi,
                       mountpoint) == 3 && strcmp(mountpoint, "/") == 0 &&
                ab_root_identity_matches(S_ISBLK(slot_st.st_mode),
                    major(root_st.st_dev), minor(root_st.st_dev),
                    major(slot_st.st_rdev), minor(slot_st.st_rdev), ma, mi)) {
                found = 1;
            }
        }
        fclose(fp);
        if (!found) {
            set_err(errbuf, errlen, "root mountinfo identity missing or mismatched");
            return -1;
        }
    }

    }

    *out_slot = cmdline_slot;
    return 0;
}

int ab_detect_running_slot_with_mountinfo(const ab_health_cfg_t *cfg,
                                          const char *mountinfo_path,
                                          ab_slot_t *out_slot,
                                          char *errbuf,
                                          size_t errlen)
{
    return ab_detect_running_slot_full(cfg, mountinfo_path, 0, out_slot, errbuf, errlen);
}

int ab_detect_running_slot(const ab_health_cfg_t *cfg,
                           ab_slot_t *out_slot,
                           char *errbuf,
                           size_t errlen)
{
    return ab_detect_running_slot_with_mountinfo(cfg, "/proc/self/mountinfo",
                                               out_slot, errbuf, errlen);
}

static int check_sysroot_file(const char *prefix, const char *relpath, char *errbuf, size_t errlen)
{
    char full[PATH_MAX];
    int n = snprintf(full, sizeof(full), "%s%s", prefix ? prefix : "", relpath);

    if (n < 0 || (size_t)n >= sizeof(full)) {
        set_err(errbuf, errlen, "sysroot path too long");
        return -1;
    }
    if (access(full, F_OK) != 0) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "missing critical rootfs file '%s': %s", full, strerror(errno));
        return -1;
    }
    return 0;
}

int ab_verify_system_health(const ab_health_cfg_t *cfg,
                            const ab_env_t *env,
                            ab_slot_t *out_running_slot,
                            unsigned long long *out_supervisor_pid,
                            char *errbuf,
                            size_t errlen)
{
    ab_health_cfg_t effective;
    ab_slot_t running_slot = AB_SLOT_NONE;
    char mount_dev[128] = {0};
    int is_rw = 0;
    service_ipc_reply_t reply;

    if (!env) {
        set_err(errbuf, errlen, "null env");
        return -1;
    }

    if (cfg) {
        effective = *cfg;
    } else {
        ab_health_cfg_init(&effective);
    }

    if (ab_env_validate(env, errbuf, errlen) != 0)
        return -1;

    if (ab_detect_running_slot(&effective, &running_slot, errbuf, errlen) != 0)
        return -1;

    if (running_slot != env->active_slot) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "running slot %s disagrees with env sgw_active_slot %s",
                 ab_slot_str(running_slot), ab_slot_str(env->active_slot));
        return -1;
    }

    if (env->ab_state == AB_STATE_TRIAL && running_slot != env->pending_slot) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "running slot %s disagrees with trial sgw_pending_slot %s",
                 ab_slot_str(running_slot), ab_slot_str(env->pending_slot));
        return -1;
    }

    if (find_root_mount_info(effective.proc_mounts_path, mount_dev,
                             sizeof(mount_dev), &is_rw, errbuf, errlen) != 0)
        return -1;

    if (!is_rw) {
        set_err(errbuf, errlen, "root filesystem is mounted read-only; health gate failed");
        return -1;
    }

    if (check_sysroot_file(effective.sysroot_prefix, "/sbin/init", errbuf, errlen) != 0 ||
        check_sysroot_file(effective.sysroot_prefix, "/etc/inittab", errbuf, errlen) != 0)
        return -1;

    memset(&reply, 0, sizeof(reply));
    if (service_ipc_call(effective.supervisor_run_dir, SERVICE_IPC_STATUS,
                         effective.supervisor_timeout_ms, &reply) != 0) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "service_supervisor IPC unavailable at '%s': %s",
                 effective.supervisor_run_dir, strerror(errno));
        return -1;
    }

    if (reply.result != SERVICE_IPC_OK) {
        snprintf(errbuf, errlen ? errlen : 0,
                 "managed service unhealthy via supervisor (result=%s detail=%s)",
                 service_ipc_result_name(reply.result), reply.detail);
        return -1;
    }

    if (reply.pid <= 0 || reply.starttime == 0) {
        set_err(errbuf, errlen, "supervisor returned OK with invalid service pid/starttime");
        return -1;
    }

    if (effective.verify_supervisor_pid) {
        if (kill(reply.pid, 0) != 0 && errno == ESRCH) {
            snprintf(errbuf, errlen ? errlen : 0,
                     "reported service pid %ld is not alive", (long)reply.pid);
            return -1;
        }
    }

    /*
     * STATUS confirms only process state, not Gateway business, devices,
     * data compatibility, or a stability window.  Until a trusted provider
     * of those checks is integrated, never promote a trial to confirmed.
     */
    (void)out_running_slot;
    (void)out_supervisor_pid;
    set_err(errbuf, errlen, "trusted system and business health provider not implemented");
    return -1;
}
