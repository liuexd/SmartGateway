import os
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
            env = dict(os.environ, LD_PRELOAD=str(so))

            # 1) Production default policy via r3c_confirm_harness (ops=NULL) fails closed at real gates
            result = subprocess.run([str(ROOT / "bsp/apps/boot_ab/r3c_confirm_harness"),
                                     "confirm-null-gate"], env=env, capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn("trusted end-to-end business health unavailable", result.stderr)
            self.assertIn("untrusted application directory", result.stderr)

            # 2) Production CLI sgw_bootctl confirm also fails closed (non-root & LD_PRELOAD root)
            nonroot = subprocess.run([str(ROOT / "bsp/apps/boot_ab/sgw_bootctl"), "confirm"],
                                     capture_output=True, text=True)
            self.assertNotEqual(nonroot.returncode, 0)
            self.assertIn("root privilege required", nonroot.stderr)

            fake_root = subprocess.run([str(ROOT / "bsp/apps/boot_ab/sgw_bootctl"), "confirm"],
                                       env=env, capture_output=True, text=True)
            self.assertNotEqual(fake_root.returncode, 0)
            self.assertIn("CONFIRM REJECTED", fake_root.stderr)
            self.assertNotIn("CONFIRMED", fake_root.stdout)

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

    def test_foreign_mount_namespace_conflict_rejected_and_vanished_pid_tolerated(self):
        with tempfile.TemporaryDirectory() as temp:
            base = Path(temp)
            src = base / "ns_probe.c"
            src.write_text(
                '#include "' + str(ROOT / "bsp/apps/boot_ab/r3c_txn.c") + '"\n'
                'int main(int argc, char **argv) {\n'
                '    char err[256] = {0};\n'
                '    if (argc != 3) return 2;\n'
                '    int rc = target_conflicts_with_mountinfo_or_slots_at(\n'
                '        NULL, argv[1], argv[2], 179, 2, NULL, NULL, 0, err, sizeof(err));\n'
                '    if (rc != 0) { fprintf(stderr, "%s\\n", err); return 1; }\n'
                '    return 0;\n'
                '}\n'
            )
            exe = base / "ns_probe"
            subprocess.run(["gcc", "-O0", "-ffunction-sections", "-fdata-sections", str(src),
                            "-Wl,--gc-sections", "-lcrypto", "-o", str(exe)], check=True)

            # Build sysfs block topology for mmcblk1:
            # p1 (179:1): [100, 400), p2 target (179:2): [800, 1200), p3 overlap (179:3): [1000, 1500)
            devices = base / "devices"
            disk = devices / "mmcblk1"
            disk.mkdir(parents=True)
            (disk / "holders").mkdir()
            (disk / "slaves").mkdir()
            sysfs = base / "sysfs"
            sysfs.mkdir()
            for part_name, minor, start, size in [
                ("mmcblk1p1", 1, 2000, 400),
                ("mmcblk1p2", 2, 3000, 1200),
                ("mmcblk1p3", 3, 3500, 1000),
            ]:
                pdir = disk / part_name
                pdir.mkdir()
                (pdir / "partition").write_text(f"{minor}\n")
                (pdir / "start").write_text(f"{start}\n")
                (pdir / "size").write_text(f"{size}\n")
                (pdir / "holders").mkdir()
                (sysfs / f"179:{minor}").symlink_to(pdir)

            proc = base / "proc"
            (proc / "self/ns").mkdir(parents=True)
            (proc / "self/ns/mnt").write_text("selfns\n")
            (proc / "self/mountinfo").write_text("1 0 179:1 / / rw - ext4 /dev/mmcblk1p1 rw\n")

            # PID 104 shares self's mount namespace; even if its mountinfo points to 179:2 it is deduplicated.
            (proc / "104/ns").mkdir(parents=True)
            os.link(proc / "self/ns/mnt", proc / "104/ns/mnt")
            (proc / "104/mountinfo").write_text("2 0 179:2 / /mnt rw - ext4 /dev/mmcblk1p2 rw\n")

            # PID 103 has a distinct ns/mnt inode but exited before mountinfo could be read (ENOENT).
            (proc / "103/ns").mkdir(parents=True)
            (proc / "103/ns/mnt").write_text("vanishedns\n")

            ok_res = subprocess.run([str(exe), str(proc), str(sysfs)], capture_output=True, text=True)
            self.assertEqual(ok_res.returncode, 0, ok_res.stderr)

            # PID 101 in a foreign mount namespace mounts target 179:2 -> MUST reject.
            (proc / "101/ns").mkdir(parents=True)
            (proc / "101/ns/mnt").write_text("foreign1\n")
            (proc / "101/mountinfo").write_text("10 0 179:2 / /mnt rw - ext4 /dev/mmcblk1p2 rw\n")
            res1 = subprocess.run([str(exe), str(proc), str(sysfs)], capture_output=True, text=True)
            self.assertNotEqual(res1.returncode, 0)
            self.assertIn("currently mounted target block device", res1.stderr)
            (proc / "101/mountinfo").write_text("10 0 179:1 / / rw - ext4 /dev/mmcblk1p1 rw\n")

            # PID 105 in another foreign namespace mounts overlapping 179:3 -> MUST reject.
            (proc / "105/ns").mkdir(parents=True)
            (proc / "105/ns/mnt").write_text("foreign2\n")
            (proc / "105/mountinfo").write_text("11 0 179:3 / /data rw - ext4 /dev/mmcblk1p3 rw\n")
            res2 = subprocess.run([str(exe), str(proc), str(sysfs)], capture_output=True, text=True)
            self.assertNotEqual(res2.returncode, 0)
            self.assertIn("overlaps mounted block topology", res2.stderr)
            (proc / "105/mountinfo").write_text("11 0 179:1 / / rw - ext4 /dev/mmcblk1p1 rw\n")

            # PID 102 in another foreign namespace has unreadable mountinfo (EACCES) -> MUST fail closed.
            if os.geteuid() != 0:
                (proc / "102/ns").mkdir(parents=True)
                (proc / "102/ns/mnt").write_text("foreign3\n")
                mi102 = proc / "102/mountinfo"
                mi102.write_text("12 0 179:1 / / rw - ext4 /dev/mmcblk1p1 rw\n")
                mi102.chmod(0)
                try:
                    res3 = subprocess.run([str(exe), str(proc), str(sysfs)], capture_output=True, text=True)
                    self.assertNotEqual(res3.returncode, 0)
                    self.assertIn("cannot read mountinfo (fail-closed)", res3.stderr)
                finally:
                    mi102.chmod(0o600)


if __name__ == "__main__":
    unittest.main()
