import hashlib
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

ROOT_DIR = Path(__file__).resolve().parent.parent
SUP_BIN = ROOT_DIR / "bsp/apps/service_supervisor/service_supervisor"
S99_SCRIPT = ROOT_DIR / "bsp/apps/service_supervisor/S99smartgateway"
POST_BUILD = ROOT_DIR / "bsp/rootfs/post_build_g3.sh"


def extract_supervisor_run_script() -> str:
    txt = POST_BUILD.read_text(encoding="utf-8")
    marker = "cat > \"$TARGET_DIR/opt/smartgateway/bin/service_supervisor_run\" <<'SREOF'\n"
    idx = txt.index(marker) + len(marker)
    end = txt.index("\nSREOF\n", idx)
    return txt[idx:end] + "\n"


def supervisor_run_pids(sup_exe: Path, app_root: Path):
    pids = []
    for p in Path("/proc").iterdir():
        if not p.name.isdigit():
            continue
        try:
            exe = os.readlink(p / "exe")
            if os.path.realpath(exe) != os.path.realpath(sup_exe):
                continue
            cmd = (p / "cmdline").read_bytes().replace(b"\x00", b" ").decode("utf-8", errors="replace")
            if f"run --root {app_root}" in cmd:
                pids.append(int(p.name))
        except OSError:
            continue
    return pids


class FakeSysVInit:
    """Minimal user-space SysV init respawn harness for service_supervisor_run."""

    def __init__(self, wrapper_path: Path, env: dict):
        self.wrapper_path = wrapper_path
        self.env = env
        self.spawn_count_file = wrapper_path.parent / "spawn_count.txt"
        self.proc = None

    def start_loop(self):
        # Run a tiny supervisor-like python child-reaper daemon so the respawned child
        # directly inherits this daemon's PID as ppid and respawns on exit/SIGTERM-reap.
        helper = f"""
import os, signal, subprocess, sys, time
running = True
def stop_handler(sig, frame):
    global running
    running = False
signal.signal(signal.SIGTERM, stop_handler)
signal.signal(signal.SIGINT, stop_handler)
signal.signal(signal.SIGHUP, signal.SIG_IGN)
env = os.environ.copy()
count = 0
child = None
while running:
    if child is None or child.poll() is not None:
        if not running:
            break
        count += 1
        with open({str(self.spawn_count_file)!r}, "w") as f:
            f.write(str(count))
        child = subprocess.Popen(["/bin/sh", "-c", "SMARTGATEWAY_RESPAWN=1 exec " + {str(self.wrapper_path)!r}], env=env)
    time.sleep(0.05)
if child is not None and child.poll() is None:
    child.terminate()
    try:
        child.wait(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
"""
        self.proc = subprocess.Popen([sys.executable, "-c", helper], env=self.env)
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            if self.spawn_count_file.exists():
                return
            time.sleep(0.05)
        raise RuntimeError("fake_init failed to spawn wrapper")

    def spawn_count(self) -> int:
        if not self.spawn_count_file.exists():
            return 0
        return int(self.spawn_count_file.read_text(encoding="utf-8").strip())

    def stop(self):
        if self.proc is not None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
                self.proc.wait(timeout=5)
            self.proc = None


class TestG3InittabDigestStability(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix="sgw_inittab_stab_"))
        self.app_root = self.tmp / "opt/smartgateway"
        for d in ("bin", "run", "logs", "releases/0.6.0", "state"):
            (self.app_root / d).mkdir(parents=True, exist_ok=True)
        self.sup = self.app_root / "bin/service_supervisor"
        shutil.copy2(SUP_BIN, self.sup)
        self.sup.chmod(0o755)

        self.wrapper = self.app_root / "bin/service_supervisor_run"
        self.wrapper.write_text(extract_supervisor_run_script(), encoding="utf-8")
        self.wrapper.chmod(0o755)

        gw = self.app_root / "releases/0.6.0/smart_gateway"
        gw.write_text("#!/bin/sh\ntrap 'exit 0' TERM INT\nwhile true; do sleep 1; done\n", encoding="utf-8")
        gw.chmod(0o755)
        (self.app_root / "current").symlink_to("releases/0.6.0")
        (self.app_root / "service.conf").write_text(
            f"executable={self.app_root}/current/smart_gateway\nserial=\nbaud=115200\nserver=127.0.0.1\nport=9000\nstop_timeout_ms=2000\n",
            encoding="utf-8",
        )

        self.inittab = self.tmp / "etc_inittab"
        self.inittab.write_text(
            "id:3:initdefault:\n"
            "sgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec /opt/smartgateway/bin/service_supervisor_run'\n",
            encoding="utf-8",
        )
        self.stop_flag = self.tmp / "runtime.disabled"

    def tearDown(self):
        for pid in supervisor_run_pids(self.sup, self.app_root):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _run_s99(self, action: str, init_pid: int, expect_rc: int = 0):
        env = os.environ.copy()
        env["SMARTGATEWAY_ROOT"] = str(self.app_root)
        env["SMARTGATEWAY_INITTAB"] = str(self.inittab)
        env["SMARTGATEWAY_STOP_FLAG"] = str(self.stop_flag)
        env["SMARTGATEWAY_INIT_PID"] = str(init_pid)
        res = subprocess.run([str(S99_SCRIPT), action], env=env, capture_output=True, text=True, timeout=20)
        if expect_rc is not None:
            self.assertEqual(res.returncode, expect_rc, f"{action} rc={res.returncode}\n{res.stdout}\n{res.stderr}")
        return res

    def test_stop_start_crash_recovery_preserve_inittab_digest(self):
        if not SUP_BIN.exists():
            self.skipTest(f"host supervisor binary not built: {SUP_BIN}")

        inittab_sha_before = hashlib.sha256(self.inittab.read_bytes()).hexdigest()
        init_env = os.environ.copy()
        init_env["SMARTGATEWAY_ROOT"] = str(self.app_root)
        init_env["SMARTGATEWAY_STOP_FLAG"] = str(self.stop_flag)
        fake_init = FakeSysVInit(self.wrapper, init_env)
        fake_init.start_loop()
        try:
            # 1. Start (respawn already booted) -> running & digest unchanged
            r_start = self._run_s99("start", fake_init.proc.pid, expect_rc=0)
            self.assertIn("smartgateway started", r_start.stdout)
            r_st = self._run_s99("status", fake_init.proc.pid, expect_rc=0)
            self.assertIn("supervisor: running", r_st.stdout)
            self.assertIn(f"ppid={fake_init.proc.pid}", r_st.stdout)
            self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

            # 2. Stop -> stays stopped, runtime flag prevents respawn loop, digest unchanged
            spawns_before_stop = fake_init.spawn_count()
            r_stop = self._run_s99("stop", fake_init.proc.pid, expect_rc=0)
            self.assertIn("stopped", r_stop.stdout)
            self.assertTrue(self.stop_flag.exists())
            time.sleep(2.5)
            spawns_after_wait = fake_init.spawn_count()
            self.assertEqual(supervisor_run_pids(self.sup, self.app_root), [], "supervisor respawned while stopped!")
            self.assertLessEqual(spawns_after_wait - spawns_before_stop, 2, "rapid respawn loop detected during stop!")
            r_st_stopped = self._run_s99("status", fake_init.proc.pid, expect_rc=1)
            self.assertIn("supervisor: stopped", r_st_stopped.stdout)
            self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

            # 3. Start again -> resumes without touching inittab, digest unchanged
            r_start2 = self._run_s99("start", fake_init.proc.pid, expect_rc=0)
            self.assertIn("smartgateway started", r_start2.stdout)
            self.assertFalse(self.stop_flag.exists())
            pids = supervisor_run_pids(self.sup, self.app_root)
            self.assertEqual(len(pids), 1)
            old_pid = pids[0]
            self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

            # 4. Crash recovery (SIGKILL supervisor) -> auto-respawns new PID with ppid=fake_init
            os.kill(old_pid, signal.SIGKILL)
            new_pids = []
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                cand = supervisor_run_pids(self.sup, self.app_root)
                if cand and cand[0] != old_pid:
                    r_chk = self._run_s99("status", fake_init.proc.pid, expect_rc=None)
                    if r_chk.returncode == 0:
                        new_pids = cand
                        break
                time.sleep(0.1)
            self.assertEqual(len(new_pids), 1, "supervisor did not recover after SIGKILL")
            self.assertNotEqual(new_pids[0], old_pid)
            self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

            # 5. Stop again cleanly
            self._run_s99("stop", fake_init.proc.pid, expect_rc=0)
            self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)
        finally:
            fake_init.stop()

    def test_start_refuses_mutating_inittab_when_entry_missing(self):
        self.inittab.write_text("id:3:initdefault:\n", encoding="utf-8")
        before = hashlib.sha256(self.inittab.read_bytes()).hexdigest()
        res = self._run_s99("start", 999999, expect_rc=1)
        self.assertIn("respawn entry missing", res.stderr)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), before)


if __name__ == "__main__":
    unittest.main()
