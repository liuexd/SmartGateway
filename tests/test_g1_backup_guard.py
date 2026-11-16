#!/usr/bin/env python3
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
BACKUP_SCRIPT = REPO_ROOT / "bsp/tools/g1_emmc_backup_verify.sh"


class TestG1BackupGuard(unittest.TestCase):
    def setUp(self):
        self.tmpdir = Path(tempfile.mkdtemp(prefix="sgw_g1_guard_"))
        self.bin_dir = self.tmpdir / "bin"
        self.bin_dir.mkdir()
        self.emmc_file = self.tmpdir / "mmcblk1"
        self.emmc_file.write_bytes(b"\x55\xaa" * (4098 * 256))
        for suffix in ("p1", "p2", "p3", "boot0", "boot1"):
            (self.tmpdir / f"mmcblk1{suffix}").write_bytes(b"DATA" * 1024)

        self.mounts_file = self.tmpdir / "proc_mounts"
        self.cmdline_file = self.tmpdir / "proc_cmdline"

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def make_mock_df(self, df_dev: str, df_mnt: str = "/"):
        df_bin = self.bin_dir / "df"
        df_bin.write_text(
            f"#!/bin/sh\n"
            f"echo 'Filesystem 1024-blocks Used Available Capacity Mounted on'\n"
            f"echo '{df_dev} 1500000 700000 800000 47% {df_mnt}'\n",
            encoding="utf-8",
        )
        df_bin.chmod(0o755)

    def run_backup(self, dest: Path, mode: str = "metadata", extra_args=()):
        env = os.environ.copy()
        env["PATH"] = f"{self.bin_dir}:{env.get('PATH', '')}"
        env["EMMC_DEV"] = str(self.emmc_file)
        env["PROC_MOUNTS"] = str(self.mounts_file)
        env["PROC_CMDLINE"] = str(self.cmdline_file)
        env["SGW_ALLOW_FILE_BACKED_TEST"] = "1"
        return subprocess.run(
            ["sh", str(BACKUP_SCRIPT), "--dest", str(dest), "--mode", mode, *extra_args],
            env=env,
            capture_output=True,
            text=True,
        )

    def test_refuses_dev_root_backed_by_mmcblk1p2(self):
        self.make_mock_df("/dev/root", "/")
        self.mounts_file.write_text("/dev/root / ext4 rw,relatime 0 0\n", encoding="utf-8")
        self.cmdline_file.write_text("console=ttymxc0,115200 root=/dev/mmcblk1p2 rootwait rw\n", encoding="utf-8")
        dest = self.tmpdir / "forbidden_on_root/sub"
        res = self.run_backup(dest)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("refusing potential eMMC self-backup", res.stderr)
        self.assertFalse(dest.exists())

    def test_refuses_dev_root_when_cmdline_unproven(self):
        self.make_mock_df("/dev/root", "/")
        self.mounts_file.write_text("/dev/root / ext4 rw,relatime 0 0\n", encoding="utf-8")
        self.cmdline_file.write_text("console=ttymxc0,115200 root=PARTUUID=deadbeef\n", encoding="utf-8")
        res = self.run_backup(self.tmpdir / "out")
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("refusing potential eMMC self-backup", res.stderr)

    def test_refuses_direct_mmcblk1_partitions(self):
        self.mounts_file.write_text("/dev/mmcblk1p1 /mnt/p1 ext4 rw 0 0\n", encoding="utf-8")
        self.cmdline_file.write_text("root=/dev/mmcblk0p2\n", encoding="utf-8")
        for dev in ("/dev/mmcblk1p1", "/dev/mmcblk1p2", "/dev/mmcblk1p3"):
            self.make_mock_df(dev, "/mnt/p1")
            res = self.run_backup(self.tmpdir / "out")
            self.assertNotEqual(res.returncode, 0)
            self.assertIn("must be off-eMMC", res.stderr)

    def test_full_mode_refuses_rw_p2_and_succeeds_from_sd_card(self):
        # 1. Even if --dest is external SD (/dev/mmcblk0p1), refuse full backup while mmcblk1p2 is rw root
        self.make_mock_df("/dev/mmcblk0p1", "/mnt/sd")
        self.mounts_file.write_text("/dev/root / ext4 rw,relatime 0 0\n/dev/mmcblk0p1 /mnt/sd vfat rw 0 0\n", encoding="utf-8")
        self.cmdline_file.write_text("root=/dev/mmcblk1p2 rootwait rw\n", encoding="utf-8")
        sd_dest = self.tmpdir / "sd_backup_rw"
        res_rw = self.run_backup(sd_dest, mode="full")
        self.assertNotEqual(res_rw.returncode, 0)
        self.assertIn("mounted rw", res_rw.stderr)

        # 2. Booted from external SD card (root=/dev/mmcblk0p2), backing up eMMC onto SD succeeds & verifies SHA256
        self.mounts_file.write_text("/dev/root / ext4 rw,relatime 0 0\n/dev/mmcblk0p1 /mnt/sd vfat rw 0 0\n", encoding="utf-8")
        self.cmdline_file.write_text("root=/dev/mmcblk0p2 rootwait rw\n", encoding="utf-8")
        sd_ok_dest = self.tmpdir / "sd_backup_ok"
        res_ok = self.run_backup(sd_ok_dest, mode="full")
        self.assertEqual(res_ok.returncode, 0, res_ok.stderr)
        self.assertIn("Backup and SHA-256 verification completed", res_ok.stdout)
        self.assertIn("emmc-p2.img: OK", res_ok.stdout)
        self.assertTrue((sd_ok_dest / "emmc-reserved-pre-p1.bin").exists())
        self.assertTrue((sd_ok_dest / "emmc-p2.img").exists())
        self.assertTrue((sd_ok_dest / "SHA256SUMS").exists())


if __name__ == "__main__":
    unittest.main()
