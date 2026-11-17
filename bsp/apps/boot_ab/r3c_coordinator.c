#define _GNU_SOURCE
#include "r3c_coordinator.h"
#include "health_devices.h"
#include "health_gateway.h"
#include "health_network.h"
#include "health_supervisor.h"
#include "r3c_binding.h"
#include "r3c_data.h"
#include "r3c_txn.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <time.h>
#include <unistd.h>

static int fail(char *err, size_t len, const char *msg)
{
    if (err && len) snprintf(err, len, "%s", msg);
    return -1;
}

static uint64_t monotonic_ms(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000L);
}

static void sleep_ms(uint64_t ms)
{
    struct timespec req;
    if (ms == 0) return;
    req.tv_sec = (time_t)(ms / 1000ULL);
    req.tv_nsec = (long)((ms % 1000ULL) * 1000000L);
    while (nanosleep(&req, &req) != 0 && errno == EINTR) {}
}

static int read_boot_id(const char *path, char out[37])
{
    char buf[64];
    int fd;
    ssize_t n;
    size_t i;
    int nonzero = 0;

    if (!path) path = "/proc/sys/kernel/random/boot_id";
    fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n < 36) return -1;
    buf[n] = '\0';
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = '\0';
    if (n != 36) return -1;
    for (i = 0; i < 36; ++i) {
        char c = buf[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) {
            if (c != '-') return -1;
        } else {
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return -1;
            if (c != '0') nonzero = 1;
        }
    }
    if (!nonzero) return -1;
    memcpy(out, buf, 37);
    return 0;
}

static int verify_data_under_policy(const r3c_policy_t *p, const r3c_runtime_ops_t *ops,
                                    char *err, size_t errlen)
{
    if (ops && ops->allow_fuse_data_bypass_blk) {
        /* Isolated non-root host FUSE test mode: mountinfo must be fuseblk & contents+persistence must pass */
        struct stat dst, rst;
        int dir = open(p->data_mountpoint, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (dir < 0 || fstat(dir, &dst) != 0 || stat("/", &rst) != 0 || dst.st_dev == rst.st_dev) {
            if (dir >= 0) close(dir);
            return fail(err, errlen, "DATA mountpoint is not an isolated mount");
        }
        if (ops->mountinfo_path) {
            FILE *mfp = fopen(ops->mountinfo_path, "re");
            char *mbuf = NULL; size_t mcap = 0, mused = 0; ssize_t mn;
            char *line = NULL; int matched = -1;
            if (mfp) {
                while ((mn = getline(&line, &mcap, mfp)) > 0) {
                    char *nx = realloc(mbuf, mused + (size_t)mn + 1);
                    if (!nx) break;
                    mbuf = nx; memcpy(mbuf + mused, line, (size_t)mn);
                    mused += (size_t)mn; mbuf[mused] = '\0';
                }
                free(line); fclose(mfp);
                if (mbuf) {
                    matched = r3c_data_mountinfo_matches_fstype(
                        mbuf, p->data_mountpoint, major(dst.st_dev), minor(dst.st_dev),
                        ops->expected_data_fstype ? ops->expected_data_fstype : "fuse.ext4");
                    free(mbuf);
                }
            }
            if (matched != 0) { close(dir); return fail(err, errlen, "FUSE mountinfo identity check failed"); }
        }
        if (r3c_data_inspect_contents(dir, p->trusted_uid, p->data_min_bytes, err, errlen) != 0 ||
            r3c_data_verify_persistence(dir, p->trusted_uid, err, errlen) != 0) {
            close(dir);
            return -1;
        }
        close(dir);
        return 0;
    }

    {
        const char *excl[3] = { p->slot1_device, p->slot2_device, p->whole_disk_device };
        /* Reserve U-Boot redundant env sectors 0xE0000..0xE4000 (1792..1824 in 512B sectors) */
        const r3c_sector_range_t reserved[1] = { { 1792ULL, 32ULL } };
        r3c_data_full_policy_t fp;
        memset(&fp, 0, sizeof(fp));
        fp.mountpoint = p->data_mountpoint;
        fp.device = p->data_device;
        fp.mountinfo_path = (ops && ops->mountinfo_path) ? ops->mountinfo_path : "/proc/self/mountinfo";
        fp.sysfs_block_root = (ops && ops->sysfs_block_root) ? ops->sysfs_block_root : "/sys/dev/block";
        fp.expected_fstype = (ops && ops->expected_data_fstype) ? ops->expected_data_fstype : "ext4";
        fp.excluded_devices = excl;
        fp.excluded_count = 3;
        fp.reserved_sectors = reserved;
        fp.reserved_count = 1;
        fp.minimum_available_bytes = p->data_min_bytes;
        fp.owner = p->trusted_uid;
        fp.verify_write_persistence = 1;
        return r3c_data_inspect_full(&fp, err, errlen);
    }
}

static int open_verified_txdir(const char *path, uid_t owner)
{
    struct stat st;
    int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &st) != 0 || !S_ISDIR(st.st_mode) || st.st_uid != owner ||
        (st.st_mode & 0077)) {
        close(fd);
        return -1;
    }
    return fd;
}

static sgw_health_state_t check_gpio_health(const r3c_runtime_ops_t *ops)
{
    if (ops && ops->gpio_dev_node && ops->gpio_misc_dev &&
        ops->gpio_stats_path && ops->gpio_driver_link) {
        return sgw_health_gpio_inventory_at(ops->gpio_dev_node, ops->gpio_misc_dev,
                                            ops->gpio_stats_path, ops->gpio_driver_link, 300);
    }
    return sgw_health_gpio_inventory(300);
}

static sgw_health_state_t check_ap3216_health(const r3c_policy_t *p, const r3c_runtime_ops_t *ops)
{
    unsigned int raw = 0;
    sgw_health_state_t direct;
    if (ops && ops->ap3216_als_path && ops->ap3216_stats_path && ops->ap3216_driver_link) {
        direct = sgw_health_ap3216_read_at(ops->ap3216_als_path, ops->ap3216_stats_path,
                                           ops->ap3216_driver_link, &raw, 300);
    } else {
        direct = sgw_health_ap3216_read(&raw, 300);
    }
    if (!p->require_sensor_heartbeat) return direct;
    if (ops && ops->sensor_hb_fn)
        return sgw_health_sensor_combine(direct, ops->sensor_hb_fn(ops->ctx));
    return sgw_health_sensor_combine(direct, SGW_HEALTH_UNKNOWN);
}

static sgw_health_state_t check_network_health(const r3c_policy_t *p, const r3c_runtime_ops_t *ops)
{
    if (ops && ops->network_probe_fn) return ops->network_probe_fn(p->net_iface, ops->ctx);
    return sgw_health_network_probe(p->net_iface);
}

int r3c_coordinator_confirm(const r3c_policy_t *policy,
                            const fw_env_backend_cfg_t *fw_cfg,
                            const ab_health_cfg_t *hcfg,
                            ab_slot_t expected_running_slot,
                            const r3c_runtime_ops_t *ops,
                            r3c_confirm_result_t *out,
                            char *errbuf,
                            size_t errlen)
{
    int app_lockfd = -1, txdir = -1, tx_lockfd = -1, slotfd = -1, rc = -1;
    ab_slot_t running_slot = AB_SLOT_NONE;
    ab_env_t env, verify_env;
    r3c_txn_record_t txn;
    r3c_binding_t persisted_bind, observed_bind;
    char boot_id[37];
    sgw_health_identity_t hid;
    sgw_health_policy_t hpol;
    sgw_health_tracker_t tracker;
    sgw_health_assessment_t assess;
    uint64_t t_start, t_observed = 0, step_ms;
    unsigned long trial_bootcount = 0;
    pid_t first_gw_pid = 0;
    unsigned long long first_gw_start = 0;
    int have_first_gw = 0;

    if (!policy || !fw_cfg || !hcfg)
        return fail(errbuf, errlen, "null coordinator argument");
    if (!ops && (geteuid() != 0 || policy->trusted_uid != 0))
        return fail(errbuf, errlen, "production confirm requires root privilege");
    if (out) memset(out, 0, sizeof(*out));

    /* 1. Acquire ordered locks: first app OTA shared lock, then txdir exclusive lock */
    if (r3c_app_ota_inspect_locked(policy->app_ota_dir, policy->trusted_uid,
                                   &app_lockfd, errbuf, errlen) != 0)
        goto out;

    txdir = open_verified_txdir(policy->tx_dir, policy->trusted_uid);
    if (txdir < 0) {
        fail(errbuf, errlen, "untrusted or missing transaction directory");
        goto out;
    }
    if (r3c_txn_acquire_lock(txdir, policy->trusted_uid, &tx_lockfd, errbuf, errlen) != 0)
        goto out;

    /* 2. Verify DATA block device, slot exclusion, schema=1 and persistence */
    if (verify_data_under_policy(policy, ops, errbuf, errlen) != 0)
        goto out;

    /* 3. Verify OS running slot & caller --running BEFORE any write or transaction mutation */
    if (ab_detect_running_slot_full(
            hcfg, (ops && ops->mountinfo_path) ? ops->mountinfo_path : "/proc/self/mountinfo",
            (ops && ops->allow_host_slot_sim) ? 1 : 0,
            &running_slot, errbuf, errlen) != 0)
        goto out;
    if (expected_running_slot != AB_SLOT_NONE && expected_running_slot != running_slot) {
        fail(errbuf, errlen, "caller --running disagrees with verified OS running slot");
        goto out;
    }
    if (fw_env_backend_load(fw_cfg, &env, errbuf, errlen) != 0)
        goto out;
    if (ab_env_validate(&env, errbuf, errlen) != 0)
        goto out;

    /* 4. Load active transaction & verify slot digest or split-state recovery */
    if (r3c_txn_load_active(txdir, policy->trusted_uid, &txn, errbuf, errlen) != 0)
        goto out;

    if (env.ab_state == AB_STATE_CONFIRMED && env.upgrade_available == 0 &&
        env.bootcount == 0 && env.active_slot == running_slot &&
        env.last_good_slot == running_slot &&
        txn.state == R3C_TXN_CONFIRMING && !txn.consumed &&
        (ab_slot_t)txn.target_slot == running_slot) {
        const char *slot_path = (ops && ops->slot_image_override) ?
            ops->slot_image_override : ab_slot_device(running_slot);
        if (!slot_path) { fail(errbuf, errlen, "missing slot path"); goto out; }
        slotfd = open(slot_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (slotfd < 0) { fail(errbuf, errlen, "cannot open slot for split-state recovery"); goto out; }
        if (r3c_txn_reconcile_confirm(txdir, txn.id, policy->trusted_uid,
                                      running_slot, slotfd, errbuf, errlen) != 0)
            goto out;
        if (out) {
            snprintf(out->transaction_id, sizeof(out->transaction_id), "%s", txn.id);
            snprintf(out->boot_id, sizeof(out->boot_id), "%s", txn.boot_id);
            out->running_slot = running_slot;
            out->last_good_slot = env.last_good_slot;
            out->bootcount = txn.bootcount;
            out->overall_state = SGW_HEALTH_HEALTHY;
            out->samples = 0;
            out->recovered = 1;
        }
        rc = 0;
        goto out;
    }

    if (env.ab_state != AB_STATE_TRIAL || env.upgrade_available != 1 ||
        env.active_slot != running_slot || env.pending_slot != running_slot ||
        env.last_good_slot == running_slot || env.bootcount == 0) {
        fail(errbuf, errlen, "U-Boot environment not in valid trial state for running slot");
        goto out;
    }
    trial_bootcount = env.bootcount;

    if ((txn.state != R3C_TXN_PENDING && txn.state != R3C_TXN_CONFIRMING) || txn.consumed) {
        fail(errbuf, errlen, "transaction is not pending or already consumed");
        goto out;
    }
    if ((ab_slot_t)txn.target_slot != running_slot ||
        (ab_slot_t)txn.last_good_slot != env.last_good_slot) {
        fail(errbuf, errlen, "transaction slot mismatch with running trial environment");
        goto out;
    }

    {
        const char *slot_path = (ops && ops->slot_image_override) ?
            ops->slot_image_override : ab_slot_device(running_slot);
        char actual_hex[65];
        uint64_t actual_len = 0;
        if (!slot_path) { fail(errbuf, errlen, "missing slot path"); goto out; }
        slotfd = open(slot_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (slotfd < 0) { fail(errbuf, errlen, "cannot open installed slot for digest verify"); goto out; }
        if (r3c_txn_digest_fd(slotfd, txn.written_size, actual_hex, &actual_len) != 0 ||
            actual_len != txn.written_size || strcmp(actual_hex, txn.image_sha256) != 0) {
            fail(errbuf, errlen, "installed slot readback digest mismatch against transaction");
            goto out;
        }
    }

    /* 5. Read current boot_id & prepare binding/tracker */
    if (read_boot_id((ops && ops->boot_id_path) ? ops->boot_id_path : NULL, boot_id) != 0) {
        fail(errbuf, errlen, "cannot read kernel boot_id");
        goto out;
    }
    if (txn.state == R3C_TXN_CONFIRMING && txn.boot_id[0] &&
        strcmp(txn.boot_id, boot_id) != 0) {
        fail(errbuf, errlen, "stale CONFIRMING transaction from different boot_id");
        goto out;
    }

    memset(&persisted_bind, 0, sizeof(persisted_bind));
    snprintf(persisted_bind.transaction_id, sizeof(persisted_bind.transaction_id), "%s", txn.id);
    snprintf(persisted_bind.package_sha256, sizeof(persisted_bind.package_sha256), "%s", txn.package_sha256);
    snprintf(persisted_bind.image_sha256, sizeof(persisted_bind.image_sha256), "%s", txn.image_sha256);
    snprintf(persisted_bind.boot_id, sizeof(persisted_bind.boot_id), "%s", boot_id);
    persisted_bind.target_slot = (unsigned int)running_slot;
    persisted_bind.last_good_slot = (unsigned int)env.last_good_slot;
    persisted_bind.bootcount = env.bootcount;

    memset(&hid, 0, sizeof(hid));
    snprintf(hid.boot_id, sizeof(hid.boot_id), "%s", boot_id);
    snprintf(hid.transaction_id, sizeof(hid.transaction_id), "%s", txn.id);
    hid.slot = (unsigned int)running_slot;
    hid.bootcount = env.bootcount;

    hpol.stable_ms = policy->stable_ms;
    hpol.max_gap_ms = policy->max_gap_ms;
    hpol.deadline_ms = policy->deadline_ms;

    t_start = monotonic_ms();
    if (t_start == 0 || sgw_health_tracker_init(&tracker, &hid, &hpol, t_start) != 0) {
        fail(errbuf, errlen, "failed to initialize health tracker");
        goto out;
    }

    step_ms = policy->max_gap_ms / 4;
    if (step_ms < 20) step_ms = 20;
    if (step_ms > 200) step_ms = 200;

    /* 6. Stable multi-sample observation loop */
    memset(&assess, 0, sizeof(assess));
    for (;;) {
        uint64_t now = monotonic_ms(), sample_end;
        pid_t gw_pid = 0, gw_pid2 = 0;
        unsigned long long gw_start = 0, gw_start2 = 0;
        sgw_health_sample_t sample;
        char reason[128];
        int sup_ok, probe_ok, flow_ok, local_ok = 0, data_ok, app_ok;

        if (now == 0 || now < t_start) {
            fail(errbuf, errlen, "monotonic clock failure");
            goto out;
        }
        memset(&sample, 0, sizeof(sample));
        sample.identity = hid;
        sample.system = SGW_HEALTH_HEALTHY;

        sup_ok = (sgw_health_supervisor_status(policy->supervisor_sock,
                                               policy->supervisor_exe,
                                               policy->gateway_exe,
                                               policy->trusted_uid,
                                               1000, &gw_pid, &gw_start) == 0);
        if (sup_ok) {
            if (!have_first_gw) {
                first_gw_pid = gw_pid;
                first_gw_start = gw_start;
                have_first_gw = 1;
            } else if (gw_pid != first_gw_pid || gw_start != first_gw_start) {
                sup_ok = 0; /* Gateway restarted/replaced during observation */
            }
        }
        sample.supervisor = sup_ok ? SGW_HEALTH_HEALTHY : SGW_HEALTH_UNKNOWN;

        probe_ok = sup_ok &&
            (sgw_health_gateway_probe(policy->gateway_sock, policy->trusted_uid,
                                      gw_pid, gw_start, 500) == 0);
        flow_ok = sup_ok &&
            (sgw_health_gateway_flow_probe(policy->gateway_sock, policy->trusted_uid,
                                           gw_pid, gw_start, 500) == 0);
        if (policy->gateway_flow_mode == R3C_GW_CONTROLLED_LOOPBACK) {
            local_ok = sup_ok &&
                (sgw_health_gateway_local_probe(policy->gateway_sock, policy->trusted_uid,
                                                gw_pid, gw_start, 500) == 0);
        }
        /* Recheck supervisor immediately after probing to catch TOCTOU replacement */
        if (sup_ok &&
            (sgw_health_supervisor_status(policy->supervisor_sock,
                                          policy->supervisor_exe,
                                          policy->gateway_exe,
                                          policy->trusted_uid,
                                          1000, &gw_pid2, &gw_start2) != 0 ||
             gw_pid2 != gw_pid || gw_start2 != gw_start)) {
            sup_ok = 0;
            sample.supervisor = SGW_HEALTH_UNKNOWN;
        }

        if (!sup_ok || !probe_ok) {
            sample.gateway = SGW_HEALTH_UNKNOWN;
            sample.external = SGW_HEALTH_UNKNOWN;
        } else if (policy->gateway_flow_mode == R3C_GW_STRICT_FORWARD) {
            sample.gateway = flow_ok ? SGW_HEALTH_HEALTHY : SGW_HEALTH_UNKNOWN;
            sample.external = flow_ok ? SGW_HEALTH_HEALTHY : SGW_HEALTH_UNKNOWN;
        } else {
            sample.gateway = local_ok ? SGW_HEALTH_HEALTHY : SGW_HEALTH_UNHEALTHY;
            sample.external = flow_ok ? SGW_HEALTH_HEALTHY : SGW_HEALTH_DEGRADED;
        }

        sample.gpio = check_gpio_health(ops);
        sample.i2c = check_ap3216_health(policy, ops);
        sample.network = check_network_health(policy, ops);

        data_ok = (verify_data_under_policy(policy, ops, reason, sizeof(reason)) == 0);
        app_ok = (r3c_app_ota_inspect(policy->app_ota_dir, policy->trusted_uid,
                                       reason, sizeof(reason)) == 0);
        sample.data = (data_ok && app_ok) ? SGW_HEALTH_HEALTHY : SGW_HEALTH_UNHEALTHY;

        sample_end = monotonic_ms();
        if (sample_end == 0 || sample_end < now || (sample_end - now) > policy->max_gap_ms) {
            fail(errbuf, errlen, "health probe stalled beyond max_gap_ms");
            goto out;
        }
        sample.time_ms = sample_end;
        if (sgw_health_tracker_sample(&tracker, &sample, &assess) != 0) {
            fail(errbuf, errlen, "health tracker rejected inconsistent sample");
            goto out;
        }
        t_observed = sample_end;

        if (assess.timed_out) {
            fail(errbuf, errlen, "health observation timed out before stability");
            goto out;
        }
        if (assess.state == SGW_HEALTH_UNHEALTHY) {
            fail(errbuf, errlen, "required local subsystem reported UNHEALTHY");
            goto out;
        }
        if (assess.stable) {
            break;
        }
        sleep_ms(step_ms);
    }

    /* 7. Enforce policy decision (mechanism vs policy) */
    if (!assess.stable || !assess.local_checks_passed) {
        fail(errbuf, errlen, "system health not stable or local checks failed");
        goto out;
    }
    if (assess.state != SGW_HEALTH_HEALTHY &&
        !(assess.state == SGW_HEALTH_DEGRADED && policy->allow_external_degraded)) {
        fail(errbuf, errlen, "external health policy forbids confirm while degraded/unknown");
        goto out;
    }

    /* 8. Final binding freshness check under both held locks right before committing */
    {
        char cur_boot[37];
        uint64_t now_commit = monotonic_ms();
        if (read_boot_id((ops && ops->boot_id_path) ? ops->boot_id_path : NULL, cur_boot) != 0) {
            fail(errbuf, errlen, "cannot re-read boot_id before commit");
            goto out;
        }
        observed_bind = persisted_bind;
        snprintf(observed_bind.boot_id, sizeof(observed_bind.boot_id), "%s", cur_boot);
        if (r3c_binding_matches(&persisted_bind, &observed_bind, t_observed, now_commit,
                                policy->max_evidence_age_ms) != 0) {
            fail(errbuf, errlen, "binding or evidence freshness check failed before commit");
            goto out;
        }
    }

    /* 9. Crash-safe confirm write sequence: CONFIRMING -> U-Boot env -> env readback -> CONFIRMED(consumed=1) */
    if (r3c_txn_begin_confirm(txdir, txn.id, policy->trusted_uid,
                              running_slot, env.last_good_slot,
                              boot_id, trial_bootcount, slotfd,
                              errbuf, errlen) != 0)
        goto out;

    if (ab_confirm_trial(&env, running_slot, 1, errbuf, errlen) != 0)
        goto out;
    if (fw_env_backend_save(fw_cfg, &env, errbuf, errlen) != 0)
        goto out;
    if (fw_env_backend_load(fw_cfg, &verify_env, errbuf, errlen) != 0)
        goto out;
    if (verify_env.ab_state != AB_STATE_CONFIRMED || verify_env.upgrade_available != 0 ||
        verify_env.bootcount != 0 || verify_env.active_slot != running_slot ||
        verify_env.last_good_slot != running_slot) {
        fail(errbuf, errlen, "fw_env confirm readback verification failed");
        goto out;
    }

    if (r3c_txn_finish_confirm(txdir, txn.id, policy->trusted_uid,
                               boot_id, trial_bootcount, errbuf, errlen) != 0)
        goto out;

    if (out) {
        snprintf(out->transaction_id, sizeof(out->transaction_id), "%s", txn.id);
        snprintf(out->boot_id, sizeof(out->boot_id), "%s", boot_id);
        out->running_slot = running_slot;
        out->last_good_slot = env.last_good_slot;
        out->bootcount = env.bootcount;
        out->overall_state = assess.state;
        out->samples = tracker.samples;
    }
    rc = 0;

out:
    if (slotfd >= 0) close(slotfd);
    if (tx_lockfd >= 0) { (void)flock(tx_lockfd, LOCK_UN); close(tx_lockfd); }
    if (txdir >= 0) close(txdir);
    if (app_lockfd >= 0) { (void)flock(app_lockfd, LOCK_UN); close(app_lockfd); }
    return rc;
}
