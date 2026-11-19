#!/usr/bin/env python3
"""Tests for offline p2-only factory recovery bundle tool (g1_restore_p2_bundle.py)."""
import hashlib
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bsp/tools"))
import g1_restore_p2_bundle as p2tool
import g1_restore_segments as seg_restore


def make_sys_block(sysroot: Path, name: str, devno: str, start: int | None, size: int,
                   parent: Path | None = None):
    node = (sysroot / "devices/platform/mmc/block" / name) if parent is None else (parent / name)
    node.mkdir(parents=True, exist_ok=True)
    (node / "dev").write_text(devno + "\n")
    (node / "size").write_text(str(size) + "\n")
    if start is not None:
        (node / "start").write_text(str(start) + "\n")
        (node / "partition").write_text("1\n")
    else:
        (node / "slaves").mkdir(exist_ok=True)
    dev_link = sysroot / "dev/block" / devno
    dev_link.parent.mkdir(parents=True, exist_ok=True)
    if dev_link.exists() or dev_link.is_symlink():
        dev_link.unlink()
    dev_link.symlink_to(node)
    return node


class TestG1RestoreP2Bundle(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="g1-p2-bundle-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

        # Use scaled 6-sector p2 geometry (3072 bytes) for fast unit tests
        self.p2_start = 2052098
        self.p2_sectors = 6
        self.p2_bytes = self.p2_sectors * 512
        for p in (
            patch.object(p2tool, "P2_START_SECTOR", self.p2_start),
            patch.object(p2tool, "P2_SECTOR_COUNT", self.p2_sectors),
            patch.object(p2tool, "P2_BYTE_SIZE", self.p2_bytes),
        ):
            p.start()
            self.addCleanup(p.stop)

        self.src_p2 = self.root / "emmc-p2.img"
        self.src_p2.write_bytes((b"FACTORY_P2_GOLDEN_" * (self.p2_bytes // 18 + 1))[: self.p2_bytes])
        self.assertEqual(self.src_p2.stat().st_size, self.p2_bytes)
        self.p2_sha = hashlib.sha256(self.src_p2.read_bytes()).hexdigest()

        self.usb_dir = self.root / "usb_src"
        self.usb_dir.mkdir()
        (self.usb_dir / "g1_sdp_ram_boot.uuu").write_text("uuu_version 1.2.91\n", encoding="utf-8")

    def test_pack_verify_dry_run_default_and_restore_raw_and_gzip(self):
        raw_bundle = self.root / "bundle-raw"
        gz_bundle = self.root / "bundle-gz"
        p2tool.cmd_pack(type("Args", (), {
            "image": str(self.src_p2),
            "out_dir": str(raw_bundle),
            "compress": "none",
            "expected_sha256": self.p2_sha,
            "label": "factory-20261117",
            "usb_rescue_dir": str(self.usb_dir),
        })())
        p2tool.cmd_pack(type("Args", (), {
            "image": str(self.src_p2),
            "out_dir": str(gz_bundle),
            "compress": "gzip",
            "expected_sha256": self.p2_sha,
            "label": "factory-20261117-gz",
            "usb_rescue_dir": None,
        })())

        self.assertTrue((raw_bundle / "usb-rescue/g1_sdp_ram_boot.uuu").is_file())
        self.assertTrue((raw_bundle / "tools/g1_restore_p2_bundle.py").is_file())
        self.assertTrue((raw_bundle / "RECOVERY-RUNBOOK.md").is_file())

        target = self.root / "target_p2.img"
        dirty = b"\xAA" * self.p2_bytes
        target.write_bytes(dirty)

        # Default (no --confirm-restore-p2): preflight only, ZERO writes
        p2tool.cmd_restore(type("Args", (), {
            "bundle_dir": str(raw_bundle),
            "target_part": str(target),
            "confirm_restore_p2": False,
            "no_toggle_ro": False,
            "allow_regular_file": True,
            "mountinfo": Path("/proc/self/mountinfo"),
            "sysroot": Path("/sys"),
        })())
        self.assertEqual(target.read_bytes(), dirty)

        # Explicit --confirm-restore-p2 restores raw bundle byte-for-byte
        p2tool.cmd_restore(type("Args", (), {
            "bundle_dir": str(raw_bundle),
            "target_part": str(target),
            "confirm_restore_p2": True,
            "no_toggle_ro": False,
            "allow_regular_file": True,
            "mountinfo": Path("/proc/self/mountinfo"),
            "sysroot": Path("/sys"),
        })())
        self.assertEqual(target.read_bytes(), self.src_p2.read_bytes())

        # Re-dirty and restore from gzipped bundle -> verifies decompressed stream & writes raw bytes
        target.write_bytes(dirty)
        p2tool.cmd_restore(type("Args", (), {
            "bundle_dir": str(gz_bundle),
            "target_part": str(target),
            "confirm_restore_p2": True,
            "no_toggle_ro": False,
            "allow_regular_file": True,
            "mountinfo": Path("/proc/self/mountinfo"),
            "sysroot": Path("/sys"),
        })())
        self.assertEqual(target.read_bytes(), self.src_p2.read_bytes())

    def test_isolated_disk_rehearsal_leaves_p1_p3_and_uboot_env_untouched(self):
        # Virtual disk layout: [pre-p1 (env @0xE0000/0xE2000)] [p1] [p2] [p3]
        bundle = self.root / "bundle-rehearse"
        p2tool.cmd_pack(type("Args", (), {
            "image": str(self.src_p2),
            "out_dir": str(bundle),
            "compress": "none",
            "expected_sha256": self.p2_sha,
            "label": "rehearsal",
            "usb_rescue_dir": None,
        })())

        pre_p1 = b"PREP1_UBOOT_ENV_" * 256  # 4096 bytes
        p1_data = b"SLOT1_CANDIDATE_" * 256
        p2_dirty = b"NEW_G4_CORRUPT__" * (self.p2_bytes // 16)
        p3_data = b"SHARED_DATA_P3__" * 256

        p2_slice = self.root / "p2_slice.img"
        p2_slice.write_bytes(p2_dirty)
        p2tool.cmd_restore(type("Args", (), {
            "bundle_dir": str(bundle),
            "target_part": str(p2_slice),
            "confirm_restore_p2": True,
            "no_toggle_ro": False,
            "allow_regular_file": True,
            "mountinfo": Path("/proc/self/mountinfo"),
            "sysroot": Path("/sys"),
        })())
        disk_after = pre_p1 + p1_data + p2_slice.read_bytes() + p3_data
        off_p1 = len(pre_p1)
        off_p2 = off_p1 + len(p1_data)
        off_p3 = off_p2 + self.p2_bytes
        self.assertEqual(disk_after[:off_p1], pre_p1)
        self.assertEqual(disk_after[off_p1:off_p2], p1_data)
        self.assertEqual(disk_after[off_p2:off_p3], self.src_p2.read_bytes())
        self.assertEqual(disk_after[off_p3:], p3_data)

    def test_reject_corrupt_bundle_and_decompressed_mismatch(self):
        gz_bundle = self.root / "bundle-corrupt"
        p2tool.cmd_pack(type("Args", (), {
            "image": str(self.src_p2),
            "out_dir": str(gz_bundle),
            "compress": "gzip",
            "expected_sha256": self.p2_sha,
            "label": "corrupt-check",
            "usb_rescue_dir": None,
        })())
        target = self.root / "target_corrupt.img"
        target.write_bytes(b"\x55" * self.p2_bytes)

        # Corrupt gzipped image while updating SHA256SUMS -> decompressed raw_sha256 check catches it!
        import gzip as gz_mod
        (gz_bundle / "emmc-p2.img.gz").write_bytes(gz_mod.compress(b"WRONG_DECOMPRESSED" * (self.p2_bytes // 18)))
        new_gz_sha, _ = p2tool.sha256_file(gz_bundle / "emmc-p2.img.gz")
        sums_lines = (gz_bundle / "SHA256SUMS").read_text(encoding="ascii").splitlines()
        updated = [
            f"{new_gz_sha}  emmc-p2.img.gz" if line.endswith("  emmc-p2.img.gz") else line
            for line in sums_lines
        ]
        (gz_bundle / "SHA256SUMS").write_text("\n".join(updated) + "\n", encoding="ascii")

        with self.assertRaises(p2tool.P2BundleError):
            p2tool.cmd_restore(type("Args", (), {
                "bundle_dir": str(gz_bundle),
                "target_part": str(target),
                "confirm_restore_p2": True,
                "no_toggle_ro": False,
                "allow_regular_file": True,
                "mountinfo": Path("/proc/self/mountinfo"),
                "sysroot": Path("/sys"),
            })())
        self.assertEqual(target.read_bytes(), b"\x55" * self.p2_bytes)

    def test_validate_p2_block_target_guards(self):
        sysroot = self.root / "sys"
        emmc = make_sys_block(sysroot, "mmcblk1", "179:0", None, 7634944)
        make_sys_block(sysroot, "mmcblk1p1", "179:1", 4098, 2048000, parent=emmc)
        make_sys_block(sysroot, "mmcblk1p2", "179:2", self.p2_start, self.p2_sectors, parent=emmc)
        make_sys_block(sysroot, "mmcblk1p3", "179:3", 5124098, 1024000, parent=emmc)
        sda = make_sys_block(sysroot, "sda", "8:0", None, 1000)
        make_sys_block(sysroot, "sda2", "8:2", self.p2_start, self.p2_sectors, parent=sda)

        ram_mi = self.root / "ram_mi"
        ram_mi.write_text("1 1 0:2 / / rw - rootfs rootfs rw\n", encoding="utf-8")
        mounted_p2_mi = self.root / "mnt_p2_mi"
        mounted_p2_mi.write_text("1 1 0:2 / / rw - rootfs rootfs rw\n2 1 179:2 / /mnt rw - ext4 /dev/mmcblk1p2 rw\n")
        root_p1_mi = self.root / "root_p1_mi"
        root_p1_mi.write_text("1 1 179:1 / / rw - ext4 /dev/mmcblk1p1 rw\n")

        dev_map = {
            "/dev/mmcblk1": (179, 0),
            "/dev/mmcblk1p1": (179, 1),
            "/dev/mmcblk1p2": (179, 2),
            "/dev/mmcblk1p3": (179, 3),
            "/dev/sda2": (8, 2),
        }

        def fake_dev_info(path):
            return dev_map[str(path)]

        def fake_blk_size(fd, path, st, sr):
            return self.p2_bytes

        with patch.object(p2tool.source, "device_info", side_effect=fake_dev_info), \
             patch.object(p2tool.os, "open", return_value=999), \
             patch.object(p2tool.os, "close", return_value=None), \
             patch.object(p2tool.os, "fstat", return_value=os.stat(self.src_p2)), \
             patch.object(p2tool.restore_seg, "block_size_bytes", side_effect=fake_blk_size):

            # Valid RAM rescue + unmounted mmcblk1p2 passes
            dev = p2tool.validate_p2_block_target(Path("/dev/mmcblk1p2"), self.p2_bytes, ram_mi, sysroot)
            self.assertEqual(dev, (179, 2))

            # Mounted p2 rejected
            with self.assertRaises(p2tool.P2BundleError):
                p2tool.validate_p2_block_target(Path("/dev/mmcblk1p2"), self.p2_bytes, mounted_p2_mi, sysroot)

            # Parent mmcblk1 backs active '/' rejected
            with self.assertRaises(p2tool.P2BundleError):
                p2tool.validate_p2_block_target(Path("/dev/mmcblk1p2"), self.p2_bytes, root_p1_mi, sysroot)

            # Whole disk, wrong partition (p1/p3), wrong disk (sda2) all rejected
            for bad_dev in ("/dev/mmcblk1", "/dev/mmcblk1p1", "/dev/mmcblk1p3", "/dev/sda2"):
                with self.assertRaises(p2tool.P2BundleError):
                    p2tool.validate_p2_block_target(Path(bad_dev), self.p2_bytes, ram_mi, sysroot)


if __name__ == "__main__":
    unittest.main()
