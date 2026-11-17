#!/usr/bin/env python3
import json
import os
import subprocess
import tarfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
KERNEL_BUILD = Path(os.environ.get("KERNEL_G3_BUILD_DIR", str(REPO_ROOT / "build/kernel-g3")))
ROOTFS_BUILD = Path(os.environ.get("ROOTFS_G3_BUILD_DIR", str(REPO_ROOT / "build/buildroot-g3")))


class TestG3KernelAndRootfs(unittest.TestCase):
    def test_kernel_baseline_vs_trimmed_metrics_and_constraints(self):
        mfile = KERNEL_BUILD / "g3_kernel_metrics.json"
        self.assertTrue(mfile.exists(), f"Missing {mfile}; run bsp/kernel/build_kernel_g3.sh first")
        data = json.loads(mfile.read_text(encoding="utf-8"))
        base = data["baseline"]
        trim = data["trimmed"]

        # Reproducible size & CMA improvements
        self.assertLess(trim["zImage_bytes"], base["zImage_bytes"])
        self.assertLess(trim["vmlinux_text"], base["vmlinux_text"])
        self.assertLess(trim["modules_bytes"], base["modules_bytes"])
        self.assertEqual(base["cma_mib"], 320)
        self.assertEqual(trim["cma_mib"], 32)
        self.assertTrue(trim["ap3216_in_dtb"])

        # External BSP driver modules must compile cleanly against trimmed kernel
        expected_bsp = {"bsp_hello.ko", "devm_demo.ko", "dt_match_demo.ko", "sgw_ap3216.ko", "sgw_gpio_event.ko"}
        self.assertTrue(expected_bsp.issubset(set(trim["bsp_ko_built"])))

        # Config diff must exist and reflect mandatory kept & trimmed symbols
        diff_file = KERNEL_BUILD / "g3_kernel_config.diff"
        self.assertTrue(diff_file.exists())
        self.assertGreater(diff_file.stat().st_size, 1000)

        trim_cfg = (KERNEL_BUILD / "trimmed/out/kernel.config").read_text(encoding="utf-8")
        for kept in (
            "CONFIG_SOC_IMX6ULL=y",
            "CONFIG_IMX2_WDT=y",
            "CONFIG_MMC_SDHCI_ESDHC_IMX=y",
            "CONFIG_EXT4_FS=y",
            "CONFIG_USB_CONFIGFS_F_FS=y",
            "CONFIG_FEC=y",
            "CONFIG_WLAN_VENDOR_REALTEK=y",
            "CONFIG_GPIO_MXC=y",
            "CONFIG_I2C_IMX=y",
            "CONFIG_CRYPTO_SHA256=y",
            "CONFIG_PERF_EVENTS=y",
        ):
            self.assertIn(kept + "\n", trim_cfg)

        for absent_sym in ("CONFIG_UBIFS_FS=y", "CONFIG_JFFS2_FS=y", "CONFIG_TOUCHSCREEN_GT9XX=y"):
            self.assertNotIn(absent_sym + "\n", trim_cfg)

        for trimmed_off in (
            "# CONFIG_DRM is not set",
            "# CONFIG_FB is not set",
            "# CONFIG_SOUND is not set",
            "# CONFIG_FTRACE is not set",
            "# CONFIG_BT is not set",
            "# CONFIG_MTD is not set",
            "# CONFIG_NFS_FS is not set",
            "# CONFIG_SOC_IMX6Q is not set",
        ):
            self.assertIn(trimmed_off + "\n", trim_cfg)

    def test_rootfs_trimmed_image_contents_and_bounds(self):
        mfile = ROOTFS_BUILD / "g3_rootfs_metrics.json"
        self.assertTrue(mfile.exists(), f"Missing {mfile}; run bsp/rootfs/build_rootfs_g3.sh first")
        data = json.loads(mfile.read_text(encoding="utf-8"))

        # Trimmed ext4 (128M) and payload must easily fit inside p1 (1000M) and G0 baseline (677M used)
        self.assertEqual(data["ext4_image_bytes"], 128 * 1024 * 1024)
        self.assertLess(data["payload_files_bytes"], 200 * 1024 * 1024)
        self.assertLess(data["trimmed_defconfig_package_count"], data["baseline_core_defconfig_package_count"])
        self.assertLess(data["trimmed_br2_package_count"], data["baseline_core_resolved_package_count"])
        self.assertLess(data["payload_files_bytes"], 35 * 1024 * 1024)

        tar_path = ROOTFS_BUILD / "images/rootfs.tar"
        ext4_path = ROOTFS_BUILD / "images/rootfs.ext4"
        self.assertTrue(tar_path.exists())
        self.assertTrue(ext4_path.exists())

        with tarfile.open(tar_path, "r") as tf:
            names = {m.name.lstrip("./") for m in tf.getmembers()}
            for req in (
                "boot/zImage",
                "boot/100ask_imx6ull-14x14.dtb",
                "usr/bin/adbd",
                "usr/bin/swupdate",
                "usr/bin/sgw_bootctl",
                "usr/bin/sgw_watchdog_guard",
                "usr/bin/service_supervisor",
                "usr/bin/smart_gateway",
                "usr/bin/ota_agent",
                "sbin/fw_printenv",
                "etc/fw_env.config",
                "etc/fstab",
                "etc/init.d/S15sgw_ab_guard",
                "etc/init.d/S99adbd",
                "etc/init.d/S99smartgateway",
                "usr/bin/gdbserver",
            ):
                self.assertIn(req, names)

            self.assertTrue({"sbin/debugfs", "usr/sbin/debugfs"} & names,
                            "signed manifest verification requires debugfs")

            for bloat in ("etc/init.d/S05lvgl", "etc/init.d/S50telnet", "etc/php.ini"):
                self.assertNotIn(bloat, names)

            fstab_txt = tf.extractfile(next(m for m in tf.getmembers() if m.name.lstrip("./") == "etc/fstab")).read().decode()
            self.assertIn("ext4", fstab_txt)
            fw_cfg_txt = tf.extractfile(next(m for m in tf.getmembers() if m.name.lstrip("./") == "etc/fw_env.config")).read().decode()
            self.assertIn("0xe0000", fw_cfg_txt)
            self.assertIn("0xe2000", fw_cfg_txt)

        # Verify ext4 image filesystem lacks metadata_csum (U-Boot 2017.03 compatibility)
        tune = subprocess.run(["/sbin/tune2fs", "-l", str(ext4_path)], capture_output=True, text=True)
        if tune.returncode == 0:
            self.assertNotIn("metadata_csum", tune.stdout)


if __name__ == "__main__":
    unittest.main()
