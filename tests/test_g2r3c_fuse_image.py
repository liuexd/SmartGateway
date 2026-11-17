#!/usr/bin/env python3
"""A real ext4 image via unprivileged FUSE; never accesses a real block device."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "bsp/apps/boot_ab/r3c_probe_cli"
FUSE2FS = os.environ.get("SGW_TEST_FUSE2FS")


@unittest.skipUnless(FUSE2FS, "SGW_TEST_FUSE2FS must explicitly name test FUSE executable")
class ImageTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".sgw-r3c-image-", dir=Path.home())
        self.addCleanup(self.tmp.cleanup)
        self.base = Path(self.tmp.name)
        self.img = self.base / "data.ext4"
        self.mount = self.base / "data"
        self.mount.mkdir(mode=0o700)
        self.mounted = False
        self.addCleanup(self.unmount)
        with self.img.open("wb") as f:
            f.truncate(32 * 1024 * 1024)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", str(self.img)], check=True)
        for key, number in (("uid", os.getuid()), ("gid", os.getgid())):
            subprocess.run(["debugfs", "-w", "-R", f"set_inode_field <2> {key} {number}",
                            str(self.img)], check=True, capture_output=True)

    def do_mount(self, readonly=False):
        args = [FUSE2FS, str(self.img), str(self.mount)]
        if readonly:
            args.extend(["-o", "ro"])
        subprocess.run(args, check=True,
                       capture_output=True, timeout=10)
        self.mounted = True
        if not readonly:
            self.mount.chmod(0o700)

    def unmount(self):
        if self.mounted:
            subprocess.run(["fusermount3", "-u", str(self.mount)], check=True,
                           capture_output=True, timeout=10)
            self.mounted = False

    def probe(self, minimum=None):
        args = [str(PROBE), "contents", str(self.mount)]
        if minimum is not None:
            args.append(str(minimum))
        return subprocess.run(args,
                              capture_output=True, text=True, timeout=5).returncode

    def test_shared_image_persists_across_simulated_slot_views(self):
        self.do_mount()
        self.assertNotEqual(self.probe(), 0)  # schema missing
        schema = self.mount / ".sgw-data-schema"
        schema.write_text("schema=2\n")
        schema.chmod(0o600)
        self.assertNotEqual(self.probe(), 0)
        schema.write_text("schema=1\n")
        self.assertEqual(self.probe(), 0)
        self.assertNotEqual(self.probe(1 << 40), 0)
        (self.mount / "app-state").write_text("confirmed=v1\n")
        self.unmount()
        # The second view is a fresh mount of the same image, not real p1/p2.
        self.do_mount()
        self.assertEqual(self.probe(), 0)
        self.assertEqual((self.mount / "app-state").read_text(), "confirmed=v1\n")
        self.unmount()
        self.do_mount(readonly=True)
        self.assertNotEqual(self.probe(), 0)
        self.unmount()
        self.do_mount()
        schema = self.mount / ".sgw-data-schema"
        schema.unlink()
        schema.symlink_to("app-state")
        self.assertNotEqual(self.probe(), 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
