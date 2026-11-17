#!/usr/bin/env python3
"""Host-only safety tests: no block devices or board are touched."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
HELPER = ROOT / "bsp/tools/g1_emmc_backup_verify.py"
spec = importlib.util.spec_from_file_location("g1backup", HELPER)
g1 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g1)


class TopologyTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="g1-topology-")
        self.root = Path(self.tmp.name)
        self.sys = self.root / "sys"
        (self.sys / "dev/block").mkdir(parents=True)
        self.emmc = self.make_disk("mmcblk1", (179, 8))
        self.sd = self.make_disk("mmcblk0", (179, 0))
        self.p1 = self.make_part(self.emmc, "mmcblk1p1", (179, 9))
        self.p2 = self.make_part(self.emmc, "mmcblk1p2", (179, 10))
        self.p3 = self.make_part(self.emmc, "mmcblk1p3", (179, 11))

    def tearDown(self):
        self.tmp.cleanup()

    def register(self, path, dev):
        (path / "dev").write_text("%d:%d" % dev)
        (self.sys / "dev/block" / ("%d:%d" % dev)).symlink_to(path, target_is_directory=True)

    def make_disk(self, name, dev):
        path = self.sys / "devices/platform/host/block" / name
        (path / "slaves").mkdir(parents=True)
        self.register(path, dev)
        return path

    def make_part(self, disk, name, dev):
        path = disk / name
        path.mkdir()
        (path / "partition").write_text("1")
        self.register(path, dev)
        return path

    def test_direct_and_uuid_alias_have_same_underlying_identity(self):
        # mountinfo contains major:minor regardless of whether the source was
        # /dev/mmcblk1p2, /dev/root, or a /dev/disk/by-uuid symlink.
        self.assertEqual(g1.physical_devices((179, 10), self.sys), {self.emmc})
        mounts = g1.parse_mountinfo(["42 1 179:10 / /mnt rw - ext4 /dev/disk/by-uuid/fake rw"])
        with self.assertRaises(g1.SafetyError):
            g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)

    def test_p2_rw_at_nonroot_mount_fails(self):
        mounts = g1.parse_mountinfo(["42 1 179:10 / /mnt/p2 rw - ext4 /dev/root rw"])
        with self.assertRaisesRegex(g1.SafetyError, "mounted rw"):
            g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)

    def test_read_only_emmc_and_external_sd_allowed(self):
        mounts = g1.parse_mountinfo([
            "42 1 179:10 / /mnt/p2 ro - ext4 /dev/root ro",
            "43 1 179:0 / /mnt/sd rw - ext4 /dev/mmcblk0 rw",
        ])
        g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)
        self.assertEqual(g1.physical_devices((179, 0), self.sys), {self.sd})

    def test_unknown_and_overlay_fail_closed(self):
        with self.assertRaises(g1.SafetyError):
            g1.physical_devices((7, 42), self.sys)
        mounts = g1.parse_mountinfo(["42 1 0:30 / / rw - overlay overlay rw"])
        with self.assertRaises(g1.SafetyError):
            g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)

    def test_ram_root_is_allowed_without_hiding_block_mounts(self):
        for kind in ("rootfs", "ramfs", "tmpfs"):
            with self.subTest(kind=kind):
                mounts = g1.parse_mountinfo([
                    "41 1 0:1 / / rw - " + kind + " rootfs rw",
                    "42 1 179:10 / /mnt/p2 ro - ext4 /dev/root ro",
                ])
                g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)
                mounts.append(((179, 10), {"rw"}, "ext4", "/other"))
                with self.assertRaises(g1.SafetyError):
                    g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)

    def test_usb_functionfs_does_not_hide_block_device_writes(self):
        mounts = g1.parse_mountinfo([
            "42 1 179:10 / /mnt/p2 ro - ext4 /dev/root ro",
            "43 1 0:20 / /dev/usb-ffs/adb rw - functionfs adb rw",
        ])
        g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)
        # The same filesystem name does not excuse an unresolved block identity.
        mounts = g1.parse_mountinfo([
            "43 1 179:99 / /dev/usb-ffs/adb rw - functionfs adb rw",
        ])
        with self.assertRaises(g1.SafetyError):
            g1.assert_no_rw_emmc(mounts, self.emmc, self.sys)

    def test_device_mapper_to_emmc_is_rejected(self):
        dm = self.sys / "devices/virtual/block/dm-0"
        (dm / "slaves").mkdir(parents=True)
        self.register(dm, (253, 0))
        (dm / "slaves/mmcblk1p2").symlink_to(self.p2, target_is_directory=True)
        self.assertEqual(g1.physical_devices((253, 0), self.sys), {self.emmc})

    def test_missing_p3_aborts_validation(self):
        (self.emmc / "size").write_text(str(g1.USER_SECTORS))
        for path, (start, size) in zip((self.p1, self.p2), list(g1.GEOMETRY.values())[:2]):
            (path / "start").write_text(str(start))
            (path / "size").write_text(str(size))
        devices = {"/dev/mmcblk1": (179, 8), "/dev/mmcblk1p1": (179, 9),
                   "/dev/mmcblk1p2": (179, 10)}
        def device(path):
            if path == "/dev/mmcblk1p3":
                raise FileNotFoundError(path)
            return devices[path]
        original_sys = g1.sys_block
        original_physical = g1.physical_devices
        def fake_sys(dev, sysroot=None):
            return original_sys(dev, self.sys)
        def fake_physical(dev, sysroot=None, seen=None):
            return original_physical(dev, self.sys, seen)
        with patch.object(g1, "device_info", side_effect=device) as info, \
             patch.object(g1, "sys_block", side_effect=fake_sys), \
             patch.object(g1, "physical_devices", side_effect=fake_physical):
            with self.assertRaises(FileNotFoundError):
                g1.validate_devices()
            self.assertEqual(info.call_args.args[0], "/dev/mmcblk1p3")

    def test_rejects_existing_target_and_missing_parent(self):
        mounts = [((179, 0), {"rw"}, "ext4", "/")]
        with self.assertRaises(g1.SafetyError):
            g1.approved_destination(self.root, self.emmc, mounts, self.sys)
        with self.assertRaises(g1.SafetyError):
            g1.approved_destination(self.root / "missing/out", self.emmc, mounts, self.sys)

    def test_destination_on_emmc_rejected_before_directory_creation(self):
        # Map the test directory's actual st_dev to a simulated eMMC partition.
        # It represents the same mount identity whether df reports a direct
        # device name, /dev/root, or a UUID alias.
        dev = os.stat(self.root).st_dev
        major_minor = (os.major(dev), os.minor(dev))
        alias = self.sys / "dev/block" / ("%d:%d" % major_minor)
        if alias.exists() or alias.is_symlink():
            alias.unlink()
        alias.symlink_to(self.p2, target_is_directory=True)
        (self.p2 / "dev").write_text("%d:%d" % major_minor)
        mounts = [(major_minor, {"rw"}, "ext4", "/")]
        target = self.root / "must-not-be-created"
        with self.assertRaisesRegex(g1.SafetyError, "protected eMMC"):
            g1.approved_destination(target, self.emmc, mounts, self.sys)
        self.assertFalse(target.exists())

    def test_malformed_mountinfo_rejected(self):
        with self.assertRaises(g1.SafetyError):
            g1.parse_mountinfo(["invalid"])
        with self.assertRaises(g1.SafetyError):
            g1.parse_mountinfo([])

    def test_copy_checks_full_length_and_readback(self):
        source = self.root / "source"
        source.write_bytes(b"source data")
        dfd = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY)
        try:
            with patch.object(g1.stat, "S_ISBLK", return_value=True):
                digest = g1.copy_exact(str(source), "backup", 0, 11, dfd,
                                       (0, 0), os.stat(self.root).st_dev)
                self.assertEqual((self.root / "backup").read_bytes(), b"source data")
                self.assertEqual(len(digest), 64)
                with self.assertRaisesRegex(g1.SafetyError, "short read"):
                    g1.copy_exact(str(source), "truncated", 0, 12, dfd,
                                  (0, 0), os.stat(self.root).st_dev)
        finally:
            os.close(dfd)

    def test_block_readonly_flag_is_required(self):
        source = self.root / "source-ro"
        source.write_bytes(b"x")
        def ioctl_value(value):
            def call(fd, request, output, mutate):
                self.assertEqual(request, 0x125e)
                output[0] = value
                return 0
            return call
        with patch.object(g1.stat, "S_ISBLK", return_value=True):
            with patch.object(g1.fcntl, "ioctl", side_effect=ioctl_value(0)):
                with self.assertRaisesRegex(g1.SafetyError, "not read-only"):
                    g1.assert_readonly(str(source), (0, 0))
            with patch.object(g1.fcntl, "ioctl", side_effect=ioctl_value(1)):
                g1.assert_readonly(str(source), (0, 0))


if __name__ == "__main__":
    unittest.main()
