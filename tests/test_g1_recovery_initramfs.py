#!/usr/bin/env python3
"""Tests for g1_prepare_recovery_initramfs.py engineering fixes."""
import gzip
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bsp/tools"))
import g1_prepare_recovery_initramfs as prep


class RecoveryInitramfsTests(unittest.TestCase):
    def test_prepares_fixed_init_sshd_and_cpio(self):
        with tempfile.TemporaryDirectory(prefix="g1-initramfs-") as td:
            root = Path(td)
            src_rootfs = root / "src_rootfs"
            (src_rootfs / "dev").mkdir(parents=True)
            (src_rootfs / "usr/bin").mkdir(parents=True)
            (src_rootfs / "init").write_text("#!/bin/sh\n[ -c /dev/console ] || mount -t devtmpfs devtmpfs /dev\n")

            src_boot = root / "src_boot"
            src_boot.mkdir()
            (src_boot / "zImage").write_bytes(b"ZIMAGE")
            (src_boot / "100ask_imx6ull-14x14.dtb").write_bytes(b"DTB")

            dst_dir = root / "out"
            tools_dir = Path(__file__).resolve().parents[1] / "bsp/tools"
            prep.prepare_rootfs(src_rootfs, dst_dir / "rootfs", tools_dir)
            archive = prep.write_boot_bundle(src_boot, dst_dir / "rootfs", dst_dir / "boot", "rescue.cpio.gz")

            init_txt = (dst_dir / "rootfs/init").read_text()
            self.assertIn("grep -q ' /dev devtmpfs ' /proc/mounts", init_txt)
            self.assertNotIn("[ -c /dev/console ] || mount -t devtmpfs", init_txt)

            ssh_txt = (dst_dir / "rootfs/usr/bin/g1_start_ssh.sh").read_text()
            self.assertIn("/usr/sbin/sshd -t -f /etc/ssh/g1_sshd_config", ssh_txt)
            self.assertIn("/usr/sbin/sshd -f /etc/ssh/g1_sshd_config", ssh_txt)

            self.assertTrue((dst_dir / "rootfs/usr/bin/g1_set_emmc_ro.py").is_file())
            self.assertTrue((dst_dir / "rootfs/usr/bin/g1_restore_segments.py").is_file())
            self.assertTrue(archive.is_file())
            self.assertTrue((dst_dir / "boot/SHA256SUMS").is_file())
            raw_cpio = gzip.decompress(archive.read_bytes())
            self.assertTrue(raw_cpio.startswith(b"070701"))
            res = subprocess.run(
                ["sha256sum", "-c", "SHA256SUMS"],
                cwd=dst_dir / "boot",
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
                text=True,
                check=False,
            )
            self.assertEqual(res.returncode, 0, res.stderr)


if __name__ == "__main__":
    unittest.main()
