#!/usr/bin/env python3
"""Tests for fail-closed G1 segmented restore and readback verification."""
import hashlib
import os
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bsp/tools"))
import g1_restore_segments as restore


def make_sys_block(sysroot: Path, name: str, devno: str, start: int | None, size: int,
                   parent: Path | None = None, force_ro: str | None = None):
    if parent is None:
        node = sysroot / "devices/platform/mmc/block" / name
    else:
        node = parent / name
    node.mkdir(parents=True, exist_ok=True)
    (node / "dev").write_text(devno + "\n")
    (node / "size").write_text(str(size) + "\n")
    if start is not None:
        (node / "start").write_text(str(start) + "\n")
        (node / "partition").write_text("1\n")
    else:
        (node / "slaves").mkdir(exist_ok=True)
    if force_ro is not None:
        (node / "force_ro").write_text(force_ro)
    dev_link = sysroot / "dev/block" / devno
    dev_link.parent.mkdir(parents=True, exist_ok=True)
    if dev_link.exists() or dev_link.is_symlink():
        dev_link.unlink()
    dev_link.symlink_to(node)
    return node


class RestoreSegmentsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="g1-restore-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.backup = self.root / "backup"
        self.backup.mkdir()
        self.geometry = {"mmcblk1p1": (1, 2), "mmcblk1p2": (3, 3), "mmcblk1p3": (6, 1)}
        for p in (
            patch.object(restore.source, "GEOMETRY", self.geometry),
            patch.object(restore.source, "PRE_SECTORS", 1),
            patch.object(restore.source, "USER_SECTORS", 8),
            patch.object(restore.source, "BOOT_SECTORS", 2),
            patch.object(restore, "PROTECTED_P2_GEOM", (3, 3)),
        ):
            p.start()
            self.addCleanup(p.stop)

        tasks, _ = restore.verify_seg.layout()
        for idx, (name, size) in enumerate(tasks):
            (self.backup / name).write_bytes(bytes([0x20 + idx]) * size)
        pre = bytearray((self.backup / "emmc-reserved-pre-p1.bin").read_bytes())
        pre[446:510] = bytes(64)
        for i, ((_, (start, size)), typ) in enumerate(zip(self.geometry.items(), (0x83, 0x83, 0x0C))):
            pre[446 + 16 * i + 4] = typ
            struct.pack_into("<II", pre, 446 + 16 * i + 8, start, size)
        pre[510:512] = b"\x55\xaa"
        (self.backup / "emmc-reserved-pre-p1.bin").write_bytes(pre)
        lines = [hashlib.sha256((self.backup / name).read_bytes()).hexdigest() + "  " + name
                 for name, _ in tasks]
        (self.backup / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="ascii")

        self.sysroot = self.root / "sys"
        emmc = make_sys_block(self.sysroot, "mmcblk1", "179:0", None, 8)
        make_sys_block(self.sysroot, "mmcblk1p1", "179:1", 1, 2, parent=emmc)
        make_sys_block(self.sysroot, "mmcblk1p2", "179:2", 3, 3, parent=emmc)
        make_sys_block(self.sysroot, "mmcblk1p3", "179:3", 6, 1, parent=emmc)
        make_sys_block(self.sysroot, "mmcblk1boot0", "179:8", None, 2, parent=emmc, force_ro="1\n")
        self.mountinfo = self.root / "mountinfo"
        self.mountinfo.write_text("1 1 179:2 / / rw,relatime - ext4 /dev/mmcblk1p2 rw\n")

    def test_full_disk_restore_onto_regular_files_matches_segments_byte_for_byte(self):
        disk = self.root / "restored.img"
        boot0 = self.root / "boot0.img"
        boot1 = self.root / "boot1.img"
        restore.restore_full_disk(
            self.backup, disk, boot0, boot1,
            allow_regular_file=True, dry_run=False, confirm_overwrite_all=True,
            mountinfo_path=self.mountinfo, sysroot=self.sysroot,
        )
        expected_user = b"".join(
            (self.backup / name).read_bytes() for name, _ in restore.verify_seg.layout()[0][:5]
        )
        self.assertEqual(disk.read_bytes(), expected_user)
        self.assertEqual(boot0.read_bytes(), (self.backup / "emmc-boot0.img").read_bytes())
        self.assertEqual(boot1.read_bytes(), (self.backup / "emmc-boot1.img").read_bytes())
        header = disk.read_bytes()[:512]
        restore.verify_seg.verify_mbr(header)

    def test_single_segment_refuses_p2_and_symlinks_and_detects_readback_corruption(self):
        p2_part = self.root / "fake_p2"
        p2_part.write_bytes(b"\x00" * 1024)
        with self.assertRaises(restore.RestoreError):
            restore.restore_single_segment(
                self.backup, "emmc-p2.img", p2_part, None, None,
                True, False, self.mountinfo, self.sysroot,
            )

        link = self.root / "link.img"
        link.symlink_to(p2_part)
        with self.assertRaises(restore.RestoreError):
            restore.restore_single_segment(
                self.backup, "emmc-p1.img", link, None, None,
                True, False, self.mountinfo, self.sysroot,
            )

        # Simulate protected p2 block device identity rejection
        blocked, reason = restore.is_protected_p2_or_active(
            (179, 2), self.sysroot, *restore.mounted_physical_targets(
                restore.source.parse_mountinfo(self.mountinfo.read_text().splitlines()), self.sysroot
            )
        )
        self.assertTrue(blocked)
        self.assertIn("mounted", reason)

        # Unmount p2 in mountinfo, verify geometry check still protects mmcblk1p2
        ram_mount = self.root / "ram_mountinfo"
        ram_mount.write_text("1 1 0:2 / / rw - rootfs rootfs rw\n")
        blocked2, reason2 = restore.is_protected_p2_or_active(
            (179, 2), self.sysroot, *restore.mounted_physical_targets(
                restore.source.parse_mountinfo(ram_mount.read_text().splitlines()), self.sysroot
            )
        )
        self.assertTrue(blocked2)
        self.assertIn("p2", reason2)

        # Simulate allowed p1 single-segment restore and verify readback
        p1_target = self.root / "p1_target.img"
        p1_target.write_bytes(b"\x00" * 1024)
        restore.restore_single_segment(
            self.backup, "emmc-p1.img", p1_target, None, None,
            True, False, ram_mount, self.sysroot,
        )
        self.assertEqual(p1_target.read_bytes(), (self.backup / "emmc-p1.img").read_bytes())

    def test_force_ro_guard_toggles_and_restores(self):
        bsys = self.sysroot / "devices/platform/mmc/block/mmcblk1/mmcblk1boot0"
        self.assertEqual((bsys / "force_ro").read_text().strip(), "1")
        with restore.ForceRoGuard(bsys, dry_run=False):
            self.assertEqual((bsys / "force_ro").read_text().strip(), "0")
        self.assertEqual((bsys / "force_ro").read_text().strip(), "1")


if __name__ == "__main__":
    unittest.main()
