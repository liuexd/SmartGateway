#!/usr/bin/env python3
"""Read-only DATA prerequisites and existing application OTA integration."""
import fcntl
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
PROBE = ROOT / "bsp/apps/boot_ab/r3c_probe_cli"


class DataTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".sgw-r3c-", dir=Path.home())
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.app = self.root / "app"
        self.app.mkdir(mode=0o700)
        (self.app / "state").mkdir(mode=0o700)
        (self.app / "releases").mkdir(mode=0o700)
        (self.app / "releases/v1").mkdir(mode=0o700)
        (self.app / "releases/v1/smart_gateway").write_text("fixture\n")
        (self.app / "releases/v1/smart_gateway").chmod(0o700)
        (self.app / ".ota.lock").touch(mode=0o600)
        (self.app / "current").symlink_to("releases/v1")
        self.state("CONFIRMED")
        self.assertEqual(self.run_probe("app", self.app).returncode, 0)

    def state(self, value, schema=1):
        (self.app / "state/ota_state.json").write_text(json.dumps({
            "schema_version": schema, "state": value,
            "confirmed_version": "v1", "candidate_version": "v2"}))
        (self.app / "state/ota_state.json").chmod(0o600)

    def run_probe(self, *args, input=None):
        return subprocess.run([str(PROBE), *map(str, args)], input=input,
                              capture_output=True, text=True, timeout=5)

    def test_settled_confirmed_and_rolled_back(self):
        self.assertEqual(self.run_probe("app", self.app).returncode, 0)
        self.state("ROLLED_BACK")
        self.assertEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "last_operation.json").write_text(
            '{"format_version":1,"phase":"DONE","pid":1,"started_at":1,'
            '"boot_id":"00000000-0000-0000-0000-000000000000"}')
        (self.app / "last_operation.json").chmod(0o600)
        self.assertEqual(self.run_probe("app", self.app).returncode, 0)

    def test_incomplete_corrupt_or_untrusted_app_state(self):
        for state in ("DOWNLOADED", "STAGED", "ACTIVATED", "HEALTHY", "ROLLBACK_PENDING"):
            with self.subTest(state=state):
                self.state(state)
                self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        self.state("CONFIRMED", schema=2)
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        self.state("CONFIRMED")
        (self.app / "state/ota_state.json").write_text(
            '{"schema_version":1,"state":"STAGED","state":"CONFIRMED",'
            '"confirmed_version":"v1","candidate_version":"v2"}')
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        self.state("CONFIRMED")
        (self.app / "last_operation.json").write_text(
            '{"format_version":1,"phase":"SWITCHING","pid":1,"started_at":1,'
            '"boot_id":"00000000-0000-0000-0000-000000000000"}')
        (self.app / "last_operation.json").chmod(0o600)
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "last_operation.json").write_text("{broken")
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "last_operation.json").write_text(
            '{"format_version":1,"phase":"DONE","pid":1,"started_at":1}garbage')
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "last_operation.json").write_text(
            '{"format_version":2,"phase":"DONE","pid":1,"started_at":1}')
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "last_operation.json").write_text(
            '{"format_version":1,"phase":"DONE","phase":"SWITCHING","pid":1,"started_at":1}')
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "last_operation.json").unlink()
        (self.app / "current").unlink()
        (self.app / "current").symlink_to("releases/v2")
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "current").unlink()
        (self.app / "current").symlink_to("releases/v1")
        (self.app / "state/ota_state.json").chmod(0o666)
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)

    def test_lock_is_required_and_busy_fails_without_creating_it(self):
        lockpath = self.app / ".ota.lock"
        with lockpath.open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        lockpath.unlink()
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        self.assertFalse(lockpath.exists())

    def test_snapshot_lock_can_be_held_through_decision(self):
        proc = subprocess.Popen([str(PROBE), "hold-app", str(self.app)],
                                stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        try:
            self.assertEqual(proc.stdout.readline().strip(), "LOCKED")
            with (self.app / ".ota.lock").open("r+") as lock:
                with self.assertRaises(BlockingIOError):
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            proc.stdin.write("\n")
            proc.stdin.flush()
            self.assertEqual(proc.wait(timeout=5), 0)
            with (self.app / ".ota.lock").open("r+") as lock:
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        finally:
            if proc.poll() is None:
                proc.kill()
                proc.wait()
            proc.stdin.close()
            proc.stdout.close()
            proc.stderr.close()

    def test_missing_release_and_symlink_state_rejected(self):
        (self.app / "releases/v1/smart_gateway").unlink()
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)
        (self.app / "releases/v1/smart_gateway").write_text("fixture")
        (self.app / "releases/v1/smart_gateway").chmod(0o700)
        state = self.app / "state/ota_state.json"
        state.rename(self.app / "saved-state")
        state.symlink_to("../saved-state")
        self.assertNotEqual(self.run_probe("app", self.app).returncode, 0)

    def test_mountinfo_identity_and_mode(self):
        path = "/srv/smartgateway-data"
        valid = "45 22 179:3 / /srv/smartgateway-data rw,relatime - ext4 /dev/mmcblk1p3 rw\n"
        self.assertEqual(self.run_probe("mount", path, input=valid).returncode, 0)
        for bad in (
            valid.replace("179:3", "179:2"),
            valid.replace(" rw,relatime", " ro,relatime"),
            valid.replace("179:3 / ", "179:3 /subdir "),
            valid.replace(" - ext4 ", " - tmpfs "),
            valid + valid,
            valid.replace("/srv/smartgateway-data", "/different"),
            "corrupted mountinfo\n",
        ):
            with self.subTest(value=bad):
                self.assertNotEqual(self.run_probe("mount", path, input=bad).returncode, 0)

    def test_real_root_directory_or_character_device_is_not_data(self):
        self.assertNotEqual(self.run_probe("data", self.app, "/dev/null").returncode, 0)
        self.assertNotEqual(self.run_probe("data", "/", "/dev/null").returncode, 0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
