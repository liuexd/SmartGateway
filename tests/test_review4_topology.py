#!/usr/bin/env python3
"""Fail-closed sysfs relation checks with synthetic block topologies."""
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]

class TopologyTests(unittest.TestCase):
    def test_production_confirm_fails_closed_even_with_root_identity(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            source, so = base / "uid.c", base / "uid.so"
            source.write_text("#include <sys/types.h>\nuid_t geteuid(void){return 0;}\n")
            subprocess.run(["gcc", "-shared", "-fPIC", str(source), "-o", str(so)], check=True)
            env = dict(__import__("os").environ, LD_PRELOAD=str(so))
            result = subprocess.run([str(ROOT / "bsp/apps/boot_ab/r3c_confirm_harness"),
                                     "confirm-null-gate"], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("trusted end-to-end business health unavailable", result.stderr)

    def test_whole_disk_and_partition_reject_mapped_or_unknown_relations(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            src = base / "probe.c"
            src.write_text('#include "' + str(ROOT / "bsp/apps/boot_ab/r3c_txn.c") + '"\n'
                           'int main(int n,char **v){return n == 2 && '
                           'sysfs_relations_empty(v[1], 253, 0) == 0 ? 0 : 1;}\n')
            exe = base / "probe"
            subprocess.run(["gcc", "-O0", "-ffunction-sections", "-fdata-sections", str(src),
                            "-Wl,--gc-sections", "-lcrypto", "-o", str(exe)], check=True)
            devices = base / "devices"
            disk = devices / "dm-0"
            disk.mkdir(parents=True)
            (disk / "holders").mkdir()
            (disk / "slaves").mkdir()
            sysfs = base / "sysfs"
            sysfs.mkdir()
            link = sysfs / "253:0"
            link.symlink_to(disk)
            def accepted():
                return subprocess.run([str(exe), str(sysfs)]).returncode == 0
            self.assertTrue(accepted())
            (disk / "slaves/backing").symlink_to("/dev/null")
            self.assertFalse(accepted(), "mapped whole device with slaves must be rejected")
            (disk / "slaves/backing").unlink()
            (disk / "holders/holder").symlink_to("/dev/null")
            self.assertFalse(accepted())
            (disk / "holders/holder").unlink()
            part = disk / "dm-0p1"
            part.mkdir()
            (part / "partition").write_text("1\n")
            (part / "holders").mkdir()
            link.unlink()
            link.symlink_to(part)
            self.assertTrue(accepted())
            (disk / "slaves/backing").symlink_to("/dev/null")
            self.assertFalse(accepted(), "partition of mapped parent must be rejected")
            (disk / "slaves/backing").unlink()
            (part / "holders").rmdir()
            self.assertFalse(accepted(), "unknown topology must be rejected")

if __name__ == "__main__":
    unittest.main()
