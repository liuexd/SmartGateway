#define _GNU_SOURCE
#include "ab_health.h"
#include "fw_env_backend.h"
#include "r3c_bundle.h"
#include "r3c_coordinator.h"
#include "r3c_policy.h"
#include "r3c_txn.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct {
    sgw_health_state_t net_state;
    sgw_health_state_t hb_state;
} harness_ctx_t;

static sgw_health_state_t harness_net(const char *iface, void *ctx)
{
    const harness_ctx_t *h = (const harness_ctx_t *)ctx;
    (void)iface;
    return h ? h->net_state : SGW_HEALTH_UNKNOWN;
}

static sgw_health_state_t harness_hb(void *ctx)
{
    const harness_ctx_t *h = (const harness_ctx_t *)ctx;
    return h ? h->hb_state : SGW_HEALTH_UNKNOWN;
}

static sgw_health_state_t parse_hstate(const char *s)
{
    if (strcmp(s, "healthy") == 0) return SGW_HEALTH_HEALTHY;
    if (strcmp(s, "degraded") == 0) return SGW_HEALTH_DEGRADED;
    if (strcmp(s, "unhealthy") == 0) return SGW_HEALTH_UNHEALTHY;
    return SGW_HEALTH_UNKNOWN;
}

static int cmd_write_stage(int argc, char **argv)
{
    const char *txdir_p = NULL, *bundle_p = NULL, *pubkey_p = NULL, *slot_img_p = NULL;
    const char *fw_cfg_p = NULL, *cmdline_p = NULL, *mounts_p = NULL, *mountinfo_p = NULL, *sysroot_p = NULL;
    unsigned int target = 0, last_good = 0;
    int i, bundle = -1, txdir = -1, keyfd = -1, imgfd = -1, rc = 1;
    char txid[65] = {0}, err[256] = {0};
    fw_env_backend_cfg_t fw;
    ab_health_cfg_t hcfg;
    ab_slot_t running = AB_SLOT_NONE;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--txdir") == 0 && i + 1 < argc) txdir_p = argv[++i];
        else if (strcmp(argv[i], "--bundle") == 0 && i + 1 < argc) bundle_p = argv[++i];
        else if (strcmp(argv[i], "--pubkey") == 0 && i + 1 < argc) pubkey_p = argv[++i];
        else if (strcmp(argv[i], "--slot-img") == 0 && i + 1 < argc) slot_img_p = argv[++i];
        else if (strcmp(argv[i], "--fw-cfg") == 0 && i + 1 < argc) fw_cfg_p = argv[++i];
        else if (strcmp(argv[i], "--cmdline") == 0 && i + 1 < argc) cmdline_p = argv[++i];
        else if (strcmp(argv[i], "--mounts") == 0 && i + 1 < argc) mounts_p = argv[++i];
        else if (strcmp(argv[i], "--mountinfo") == 0 && i + 1 < argc) mountinfo_p = argv[++i];
        else if (strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) sysroot_p = argv[++i];
        else if (strcmp(argv[i], "--target") == 0 && i + 1 < argc) target = (unsigned int)atoi(argv[++i]);
        else if (strcmp(argv[i], "--last-good") == 0 && i + 1 < argc) last_good = (unsigned int)atoi(argv[++i]);
        else return 2;
    }
    if (!txdir_p || !bundle_p || !pubkey_p || !slot_img_p || !fw_cfg_p || !cmdline_p || !mounts_p || !mountinfo_p || !sysroot_p)
        return 2;

    fw_env_backend_cfg_from_env(&fw);
    fw.config_path = fw_cfg_p;
    ab_health_cfg_init(&hcfg);
    hcfg.proc_cmdline_path = cmdline_p;
    hcfg.proc_mounts_path = mounts_p;
    hcfg.sysroot_prefix = sysroot_p;

    if (ab_detect_running_slot_full(&hcfg, mountinfo_p, 1, &running, err, sizeof(err)) != 0) {
        fprintf(stderr, "STAGE DETECT ERROR: %s\n", err);
        return 1;
    }
    bundle = open(bundle_p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    txdir = open(txdir_p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    keyfd = open(pubkey_p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (bundle < 0 || txdir < 0 || keyfd < 0) {
        fprintf(stderr, "OPEN ERROR\n");
        goto out;
    }
    if (r3c_bundle_record(bundle, txdir, keyfd, geteuid(), target, last_good, txid) != 0) {
        fprintf(stderr, "BUNDLE VERIFY FAILED\n");
        goto out;
    }
    imgfd = openat(bundle, "rootfs.img", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (imgfd < 0) {
        fprintf(stderr, "OPEN IMAGE FAILED\n");
        goto out;
    }
    {
        r3c_slot_write_opts_t wopts;
        memset(&wopts, 0, sizeof(wopts));
        wopts.target_dev_override = slot_img_p;
        wopts.mountinfo_path = mountinfo_p;
        wopts.allow_regular_file_target = 1;
        wopts.allow_host_slot_sim = 1;
        if (r3c_txn_write_and_verify_slot(txdir, txid, geteuid(), imgfd,
                                          &fw, &hcfg, &wopts, err, sizeof(err)) != 0) {
            fprintf(stderr, "WRITE/READBACK FAILED: %s\n", err);
            goto out;
        }
    }
    if (r3c_txn_stage_pending(txdir, txid, geteuid(), &fw, &hcfg, running, err, sizeof(err)) != 0) {
        fprintf(stderr, "STAGE PENDING FAILED: %s\n", err);
        goto out;
    }
    printf("STAGED tx=%s target=%u last_good=%u\n", txid, target, last_good);
    rc = 0;
out:
    if (imgfd >= 0) close(imgfd);
    if (keyfd >= 0) close(keyfd);
    if (txdir >= 0) close(txdir);
    if (bundle >= 0) close(bundle);
    return rc;
}

static int cmd_confirm(int argc, char **argv)
{
    const char *policy_p = NULL, *fw_cfg_p = NULL, *cmdline_p = NULL, *mounts_p = NULL;
    const char *mountinfo_p = NULL, *sysroot_p = NULL, *slot_img_p = NULL, *boot_id_p = NULL;
    const char *gpio_dir = NULL, *ap_dir = NULL;
    int i, fuse_data = 0;
    ab_slot_t caller_running = AB_SLOT_NONE;
    harness_ctx_t hctx = { SGW_HEALTH_HEALTHY, SGW_HEALTH_HEALTHY };
    r3c_policy_t pol;
    fw_env_backend_cfg_t fw;
    ab_health_cfg_t hcfg;
    r3c_runtime_ops_t ops;
    r3c_confirm_result_t res;
    char err[256] = {0};
    char g_misc[1024], g_stats[1024], g_drv[1024], a_als[1024], a_stats[1024], a_drv[1024];

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--policy") == 0 && i + 1 < argc) policy_p = argv[++i];
        else if (strcmp(argv[i], "--fw-cfg") == 0 && i + 1 < argc) fw_cfg_p = argv[++i];
        else if (strcmp(argv[i], "--cmdline") == 0 && i + 1 < argc) cmdline_p = argv[++i];
        else if (strcmp(argv[i], "--mounts") == 0 && i + 1 < argc) mounts_p = argv[++i];
        else if (strcmp(argv[i], "--mountinfo") == 0 && i + 1 < argc) mountinfo_p = argv[++i];
        else if (strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) sysroot_p = argv[++i];
        else if (strcmp(argv[i], "--slot-img") == 0 && i + 1 < argc) slot_img_p = argv[++i];
        else if (strcmp(argv[i], "--boot-id-file") == 0 && i + 1 < argc) boot_id_p = argv[++i];
        else if (strcmp(argv[i], "--gpio-dir") == 0 && i + 1 < argc) gpio_dir = argv[++i];
        else if (strcmp(argv[i], "--ap3216-dir") == 0 && i + 1 < argc) ap_dir = argv[++i];
        else if (strcmp(argv[i], "--net-state") == 0 && i + 1 < argc) hctx.net_state = parse_hstate(argv[++i]);
        else if (strcmp(argv[i], "--sensor-hb") == 0 && i + 1 < argc) hctx.hb_state = parse_hstate(argv[++i]);
        else if (strcmp(argv[i], "--running") == 0 && i + 1 < argc) {
            if (ab_slot_parse(argv[++i], &caller_running) != 0) return 2;
        }
        else if (strcmp(argv[i], "--fuse-data") == 0) fuse_data = 1;
        else return 2;
    }
    if (!policy_p || !fw_cfg_p || !cmdline_p || !mounts_p || !mountinfo_p || !sysroot_p ||
        !slot_img_p || !boot_id_p || !gpio_dir || !ap_dir)
        return 2;

    if (r3c_policy_load(policy_p, geteuid(), &pol, err, sizeof(err)) != 0) {
        fprintf(stderr, "CONFIRM REJECTED: %s\n", err);
        return 1;
    }

    fw_env_backend_cfg_from_env(&fw);
    fw.config_path = fw_cfg_p;
    ab_health_cfg_init(&hcfg);
    hcfg.proc_cmdline_path = cmdline_p;
    hcfg.proc_mounts_path = mounts_p;
    hcfg.sysroot_prefix = sysroot_p;

    memset(&ops, 0, sizeof(ops));
    ops.boot_id_path = boot_id_p;
    ops.mountinfo_path = mountinfo_p;
    ops.slot_image_override = slot_img_p;
    ops.allow_fuse_data_bypass_blk = fuse_data;
    ops.allow_host_slot_sim = 1;
    snprintf(g_misc, sizeof(g_misc), "%s/misc_dev", gpio_dir);
    snprintf(g_stats, sizeof(g_stats), "%s/stats", gpio_dir);
    snprintf(g_drv, sizeof(g_drv), "%s/driver", gpio_dir);
    snprintf(a_als, sizeof(a_als), "%s/als_raw", ap_dir);
    snprintf(a_stats, sizeof(a_stats), "%s/stats", ap_dir);
    snprintf(a_drv, sizeof(a_drv), "%s/driver", ap_dir);
    ops.gpio_dev_node = "/dev/null";
    ops.gpio_misc_dev = g_misc;
    ops.gpio_stats_path = g_stats;
    ops.gpio_driver_link = g_drv;
    ops.ap3216_als_path = a_als;
    ops.ap3216_stats_path = a_stats;
    ops.ap3216_driver_link = a_drv;
    ops.network_probe_fn = harness_net;
    ops.sensor_hb_fn = harness_hb;
    ops.ctx = &hctx;

    if (r3c_coordinator_confirm(&pol, &fw, &hcfg, caller_running, &ops, &res, err, sizeof(err)) != 0) {
        fprintf(stderr, "CONFIRM REJECTED: %s\n", err);
        return 1;
    }
    printf("%s slot=%s last_good=%s tx=%s boot_id=%s state=%s samples=%u\n",
           res.recovered ? "RECOVERED_CONFIRMED" : "CONFIRMED",
           ab_slot_str(res.running_slot), ab_slot_str(res.last_good_slot),
           res.transaction_id, res.boot_id,
           sgw_health_state_name(res.overall_state), res.samples);
    return 0;
}

static int cmd_inspect_txn(int argc, char **argv)
{
    const char *txdir_p = NULL, *id = NULL;
    int i, txdir;
    r3c_txn_record_t rec;
    char err[256] = {0};

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--txdir") == 0 && i + 1 < argc) txdir_p = argv[++i];
        else if (strcmp(argv[i], "--id") == 0 && i + 1 < argc) id = argv[++i];
        else return 2;
    }
    if (!txdir_p || !id) return 2;
    txdir = open(txdir_p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (txdir < 0) return 1;
    if (r3c_txn_load_by_id(txdir, id, geteuid(), &rec, err, sizeof(err)) != 0) {
        fprintf(stderr, "TXN ERROR: %s\n", err);
        close(txdir);
        return 1;
    }
    close(txdir);
    printf("state=%s consumed=%d target=%u last_good=%u boot_id=%s bootcount=%lu\n",
           r3c_txn_state_str(rec.state), rec.consumed, rec.target_slot,
           rec.last_good_slot, rec.boot_id[0] ? rec.boot_id : "none", rec.bootcount);
    return 0;
}


static int cmd_write_slot_only(int argc, char **argv)
{
    const char *txdir_p = NULL, *txid = NULL, *image_p = NULL, *target_dev_p = NULL;
    const char *fw_cfg_p = NULL, *cmdline_p = NULL, *mounts_p = NULL, *mountinfo_p = NULL, *sysroot_p = NULL;
    int allow_reg = 1, i, txdir = -1, imgfd = -1, rc = 1;
    char err[256] = {0};
    fw_env_backend_cfg_t fw;
    ab_health_cfg_t hcfg;
    r3c_slot_write_opts_t wopts;

    for (i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--txdir") == 0 && i + 1 < argc) txdir_p = argv[++i];
        else if (strcmp(argv[i], "--txid") == 0 && i + 1 < argc) txid = argv[++i];
        else if (strcmp(argv[i], "--image") == 0 && i + 1 < argc) image_p = argv[++i];
        else if (strcmp(argv[i], "--target-dev") == 0 && i + 1 < argc) target_dev_p = argv[++i];
        else if (strcmp(argv[i], "--fw-cfg") == 0 && i + 1 < argc) fw_cfg_p = argv[++i];
        else if (strcmp(argv[i], "--cmdline") == 0 && i + 1 < argc) cmdline_p = argv[++i];
        else if (strcmp(argv[i], "--mounts") == 0 && i + 1 < argc) mounts_p = argv[++i];
        else if (strcmp(argv[i], "--mountinfo") == 0 && i + 1 < argc) mountinfo_p = argv[++i];
        else if (strcmp(argv[i], "--sysroot") == 0 && i + 1 < argc) sysroot_p = argv[++i];
        else if (strcmp(argv[i], "--require-blk") == 0) allow_reg = 0;
        else return 2;
    }
    if (!txdir_p || !txid || !image_p || !fw_cfg_p || !cmdline_p || !mounts_p || !mountinfo_p || !sysroot_p)
        return 2;

    fw_env_backend_cfg_from_env(&fw);
    fw.config_path = fw_cfg_p;
    ab_health_cfg_init(&hcfg);
    hcfg.proc_cmdline_path = cmdline_p;
    hcfg.proc_mounts_path = mounts_p;
    hcfg.sysroot_prefix = sysroot_p;

    txdir = open(txdir_p, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    imgfd = open(image_p, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (txdir < 0 || imgfd < 0) {
        fprintf(stderr, "OPEN ERROR\n");
        goto out;
    }
    memset(&wopts, 0, sizeof(wopts));
    wopts.target_dev_override = target_dev_p;
    wopts.mountinfo_path = mountinfo_p;
    wopts.allow_regular_file_target = allow_reg;
    wopts.allow_host_slot_sim = 1;
    if (r3c_txn_write_and_verify_slot(txdir, txid, geteuid(), imgfd,
                                      &fw, &hcfg, &wopts, err, sizeof(err)) != 0) {
        fprintf(stderr, "SLOT WRITE REJECTED: %s\n", err);
        goto out;
    }
    printf("SLOT_WRITTEN tx=%s\n", txid);
    rc = 0;
out:
    if (imgfd >= 0) close(imgfd);
    if (txdir >= 0) close(txdir);
    return rc;
}

int main(int argc, char **argv)
{

    if (argc < 2) return 2;
    if (strcmp(argv[1], "write-and-stage") == 0) return cmd_write_stage(argc, argv);
    if (strcmp(argv[1], "write-slot") == 0) return cmd_write_slot_only(argc, argv);
    if (strcmp(argv[1], "confirm") == 0) return cmd_confirm(argc, argv);
    if (strcmp(argv[1], "inspect-txn") == 0) return cmd_inspect_txn(argc, argv);
    return 2;
}
