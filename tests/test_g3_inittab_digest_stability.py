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
RUN_WRAPPER_SRC = ROOT_DIR / "bsp/apps/service_supervisor/service_supervisor_run"
INSTALL_SCRIPT = ROOT_DIR / "bsp/apps/service_supervisor/install.sh"


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
    """User-space SysV init respawn harness supporting SIGHUP inittab reload."""

    def __init__(self, inittab_path: Path, env: dict):
        self.inittab_path = inittab_path
        self.env = env
        self.spawn_count_file = inittab_path.parent / "spawn_count.txt"
        self.ready_file = inittab_path.parent / "init_ready"
        self.proc = None

    def start_loop(self):
        helper = f"""
import os, signal, subprocess, sys, time
from pathlib import Path

inittab = Path({str(self.inittab_path)!r})
spawn_file = Path({str(self.spawn_count_file)!r})
ready_file = Path({str(self.ready_file)!r})
running = True
reload_needed = True

def stop_handler(sig, frame):
    global running
    running = False

def hup_handler(sig, frame):
    global reload_needed
    reload_needed = True

signal.signal(signal.SIGTERM, stop_handler)
signal.signal(signal.SIGINT, stop_handler)
signal.signal(signal.SIGHUP, hup_handler)

def parse_respawn():
    if not inittab.exists():
        return None
    for line in inittab.read_text(encoding="utf-8", errors="replace").splitlines():
        s = line.strip()
        if not s or s.startswith("#"):
            continue
        parts = line.split(":", 3)
        if len(parts) == 4 and parts[2] == "respawn" and "service_supervisor" in parts[3]:
            return parts[3]
    return None

env = os.environ.copy()
count = 0
child = None
cmd = None
ready_file.write_text("1", encoding="utf-8")
while running:
    if reload_needed:
        reload_needed = False
        cmd = parse_respawn()
    if cmd is not None and (child is None or child.poll() is not None):
        if not running:
            break
        count += 1
        spawn_file.write_text(str(count), encoding="utf-8")
        child = subprocess.Popen(["/bin/sh", "-c", "exec " + cmd], env=env)
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
            if self.ready_file.exists():
                return
            time.sleep(0.05)
        raise RuntimeError("fake_init failed to start")

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
        self.tmp = Path(tempfile.mkdtemp(prefix="sgw_", dir="/tmp"))
        self.app_root = self.tmp / "gw"
        for d in ("bin", "run", "logs", "releases/0.6.0", "state"):
            (self.app_root / d).mkdir(parents=True, exist_ok=True)
        self.sup = self.app_root / "bin/service_supervisor"
        shutil.copy2(SUP_BIN, self.sup)
        self.sup.chmod(0o755)

        self.gw = self.app_root / "releases/0.6.0/smart_gateway"
        self.gw.write_text("#!/bin/sh\ntrap 'exit 0' TERM INT\nwhile true; do sleep 1; done\n", encoding="utf-8")
        self.gw.chmod(0o755)
        (self.app_root / "current").symlink_to("releases/0.6.0")

        self.inittab = self.tmp / "etc_inittab"
        self.initd = self.tmp / "S99smartgateway"
        self.stop_flag = self.tmp / "runtime.disabled"
        self.policy_dir = self.tmp / "policy"

    def tearDown(self):
        for pid in supervisor_run_pids(self.sup, self.app_root):
            try:
                os.kill(pid, signal.SIGKILL)
            except OSError:
                pass
        shutil.rmtree(self.tmp, ignore_errors=True)

    def _base_env(self, init_pid: int) -> dict:
        env = os.environ.copy()
        env["SMARTGATEWAY_ROOT"] = str(self.app_root)
        env["SMARTGATEWAY_INITTAB"] = str(self.inittab)
        env["SMARTGATEWAY_INITD"] = str(self.initd)
        env["SMARTGATEWAY_STOP_FLAG"] = str(self.stop_flag)
        env["SMARTGATEWAY_INIT_PID"] = str(init_pid)
        env["SMARTGATEWAY_POLICY_DIR"] = str(self.policy_dir)
        return env

    def _run_s99(self, s99_path: Path, action: str, init_pid: int, expect_rc: int = 0):
        res = subprocess.run([str(s99_path), action], env=self._base_env(init_pid),
                             capture_output=True, text=True, timeout=20)
        if expect_rc is not None:
            self.assertEqual(res.returncode, expect_rc, f"{action} rc={res.returncode}\n{res.stdout}\n{res.stderr}")
        return res

    def _assert_lifecycle_digest_stability(self, s99_path: Path, fake_init: FakeSysVInit):
        inittab_sha_before = hashlib.sha256(self.inittab.read_bytes()).hexdigest()

        # 1. Start -> running with ppid == fake_init
        r_start = self._run_s99(s99_path, "start", fake_init.proc.pid, expect_rc=0)
        self.assertIn("smartgateway started", r_start.stdout)
        r_st = self._run_s99(s99_path, "status", fake_init.proc.pid, expect_rc=0)
        self.assertIn("supervisor: running", r_st.stdout)
        self.assertIn(f"ppid={fake_init.proc.pid}", r_st.stdout)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

        # 2. Stop -> stays stopped, runtime flag prevents respawn loop, digest unchanged
        spawns_before_stop = fake_init.spawn_count()
        r_stop = self._run_s99(s99_path, "stop", fake_init.proc.pid, expect_rc=0)
        self.assertIn("stopped", r_stop.stdout)
        self.assertTrue(self.stop_flag.exists())
        time.sleep(2.5)
        spawns_after_wait = fake_init.spawn_count()
        self.assertEqual(supervisor_run_pids(self.sup, self.app_root), [], "supervisor respawned while stopped!")
        self.assertLessEqual(spawns_after_wait - spawns_before_stop, 2, "rapid respawn loop detected during stop!")
        r_st_stopped = self._run_s99(s99_path, "status", fake_init.proc.pid, expect_rc=1)
        self.assertIn("supervisor: stopped", r_st_stopped.stdout)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

        # 3. Start again -> resumes without touching inittab, digest unchanged
        r_start2 = self._run_s99(s99_path, "start", fake_init.proc.pid, expect_rc=0)
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
                r_chk = self._run_s99(s99_path, "status", fake_init.proc.pid, expect_rc=None)
                if r_chk.returncode == 0:
                    new_pids = cand
                    break
            time.sleep(0.1)
        self.assertEqual(len(new_pids), 1, "supervisor did not recover after SIGKILL")
        self.assertNotEqual(new_pids[0], old_pid)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

        # 5. Stop again cleanly
        self._run_s99(s99_path, "stop", fake_init.proc.pid, expect_rc=0)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha_before)

    def test_stop_start_crash_recovery_preserve_inittab_digest(self):
        if not SUP_BIN.exists():
            self.skipTest(f"host supervisor binary not built: {SUP_BIN}")

        wrapper = self.app_root / "bin/service_supervisor_run"
        shutil.copy2(RUN_WRAPPER_SRC, wrapper)
        wrapper.chmod(0o700)
        (self.app_root / "service.conf").write_text(
            f"executable={self.app_root}/current/smart_gateway\nserial=\nbaud=115200\nserver=127.0.0.1\nport=9000\nstop_timeout_ms=2000\n",
            encoding="utf-8",
        )
        self.inittab.write_text(
            f"id:3:initdefault:\nsgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec {wrapper}'\n",
            encoding="utf-8",
        )

        fake_init = FakeSysVInit(self.inittab, self._base_env(0))
        fake_init.start_loop()
        try:
            self._assert_lifecycle_digest_stability(S99_SCRIPT, fake_init)
        finally:
            fake_init.stop()

    def test_standalone_install_uses_unified_wrapper_and_stop_start_preserves_inittab(self):
        if not SUP_BIN.exists():
            self.skipTest(f"host supervisor binary not built: {SUP_BIN}")

        self.inittab.write_text("id:3:initdefault:\n", encoding="utf-8")
        fake_init = FakeSysVInit(self.inittab, self._base_env(0))
        fake_init.start_loop()
        try:
            res = subprocess.run(
                [str(INSTALL_SCRIPT), str(self.app_root)],
                env=self._base_env(fake_init.proc.pid),
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertTrue((self.app_root / "bin/service_supervisor_run").is_file())
            self.assertTrue(self.initd.is_file())
            inittab_txt = self.inittab.read_text(encoding="utf-8")
            self.assertRegex(inittab_txt, r"(?m)^sgw0::respawn:.*service_supervisor_run")
            for line in inittab_txt.splitlines():
                if line.startswith("sgw0::respawn:"):
                    self.assertLessEqual(len(line.split(":", 3)[3]), 127)
            self._assert_lifecycle_digest_stability(self.initd, fake_init)
        finally:
            fake_init.stop()

    def _file_sig(self, path: Path):
        return hashlib.sha256(path.read_bytes()).hexdigest(), path.stat().st_ino

    def test_install_on_pre_pinned_inittab_preserves_signed_digest(self):
        if not SUP_BIN.exists():
            self.skipTest(f"host supervisor binary not built: {SUP_BIN}")

        wrapper = self.app_root / "bin/service_supervisor_run"
        shutil.copy2(RUN_WRAPPER_SRC, wrapper)
        wrapper.chmod(0o700)
        shutil.copy2(S99_SCRIPT, self.initd)
        self.initd.chmod(0o755)
        self.policy_dir.mkdir(parents=True, exist_ok=True)
        (self.policy_dir / "r3c-policy.conf").write_text("gateway_flow_mode=controlled_local\n", encoding="utf-8")
        (self.app_root / "service.conf").write_text(
            f"executable={self.app_root}/current/smart_gateway\nserial=\nbaud=115200\nserver=127.0.0.1\nport=9000\n",
            encoding="utf-8",
        )
        self.inittab.write_text(
            f"id:3:initdefault:\nsgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec {wrapper}'\n",
            encoding="utf-8",
        )
        inittab_before = self._file_sig(self.inittab)
        initd_before = self._file_sig(self.initd)
        wrapper_before = self._file_sig(wrapper)

        fake_init = FakeSysVInit(self.inittab, self._base_env(0))
        fake_init.start_loop()
        try:
            res = subprocess.run(
                [str(INSTALL_SCRIPT), str(self.app_root)],
                env=self._base_env(fake_init.proc.pid),
                capture_output=True, text=True, timeout=30,
            )
            self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
            self.assertIn("跳过文件安装", res.stdout)
            self.assertEqual(self._file_sig(self.inittab), inittab_before)
            self.assertEqual(self._file_sig(self.initd), initd_before)
            self.assertEqual(self._file_sig(wrapper), wrapper_before)
            self.assertEqual(list(self.tmp.glob("etc_inittab.bak.*")), [])
        finally:
            fake_init.stop()

    def test_install_rejects_mismatched_s99_on_signed_image_without_mutating_files(self):
        if not SUP_BIN.exists():
            self.skipTest(f"host supervisor binary not built: {SUP_BIN}")

        wrapper = self.app_root / "bin/service_supervisor_run"
        conf_txt = (
            f"executable={self.app_root}/current/smart_gateway\n"
            "serial=\nbaud=115200\nserver=127.0.0.1\nport=9000\n"
        )
        (self.app_root / "service.conf").write_text(conf_txt, encoding="utf-8")
        pinned_inittab = (
            f"id:3:initdefault:\n"
            f"sgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec {wrapper}'\n"
        )

        # (a) Pre-pinned inittab + mismatched S99smartgateway (with and without policy_dir)
        for with_policy in (True, False):
            shutil.rmtree(self.policy_dir, ignore_errors=True)
            if with_policy:
                self.policy_dir.mkdir(parents=True, exist_ok=True)
                (self.policy_dir / "r3c-policy.conf").write_text("gateway_flow_mode=controlled_local\n", encoding="utf-8")
            shutil.copy2(RUN_WRAPPER_SRC, wrapper)
            wrapper.chmod(0o700)
            self.initd.write_text("#!/bin/sh\necho legacy S99smartgateway\n", encoding="utf-8")
            self.initd.chmod(0o755)
            self.inittab.write_text(pinned_inittab, encoding="utf-8")

            inittab_sha = hashlib.sha256(self.inittab.read_bytes()).hexdigest()
            initd_sha = hashlib.sha256(self.initd.read_bytes()).hexdigest()
            wrapper_sha = hashlib.sha256(wrapper.read_bytes()).hexdigest()

            res = subprocess.run(
                [str(INSTALL_SCRIPT), str(self.app_root)],
                env=self._base_env(999999),
                capture_output=True, text=True, timeout=20,
            )
            self.assertNotEqual(res.returncode, 0, f"expected failure for with_policy={with_policy}")
            self.assertIn("版本不一致", res.stderr)
            self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha)
            self.assertEqual(hashlib.sha256(self.initd.read_bytes()).hexdigest(), initd_sha)
            self.assertEqual(hashlib.sha256(wrapper.read_bytes()).hexdigest(), wrapper_sha)
            self.assertEqual(list(self.tmp.glob("etc_inittab.bak.*")), [])

        # (b) Signed policy_dir + matching S99/wrapper + legacy/mismatched inittab
        self.policy_dir.mkdir(parents=True, exist_ok=True)
        (self.policy_dir / "r3c-policy.conf").write_text("gateway_flow_mode=controlled_local\n", encoding="utf-8")
        shutil.copy2(RUN_WRAPPER_SRC, wrapper)
        wrapper.chmod(0o700)
        shutil.copy2(S99_SCRIPT, self.initd)
        self.initd.chmod(0o755)
        self.inittab.write_text(
            f"id:3:initdefault:\n::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec {self.sup} run --root {self.app_root}'\n",
            encoding="utf-8",
        )
        inittab_sha = hashlib.sha256(self.inittab.read_bytes()).hexdigest()
        initd_sha = hashlib.sha256(self.initd.read_bytes()).hexdigest()
        wrapper_sha = hashlib.sha256(wrapper.read_bytes()).hexdigest()

        res_b = subprocess.run(
            [str(INSTALL_SCRIPT), str(self.app_root)],
            env=self._base_env(999999),
            capture_output=True, text=True, timeout=20,
        )
        self.assertNotEqual(res_b.returncode, 0)
        self.assertIn("不一致", res_b.stderr)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha)
        self.assertEqual(hashlib.sha256(self.initd.read_bytes()).hexdigest(), initd_sha)
        self.assertEqual(hashlib.sha256(wrapper.read_bytes()).hexdigest(), wrapper_sha)
        self.assertEqual(list(self.tmp.glob("etc_inittab.bak.*")), [])

        # (c) Pre-pinned inittab + matching S99 + mismatched service_supervisor_run
        self.inittab.write_text(pinned_inittab, encoding="utf-8")
        wrapper.write_text("#!/bin/sh\n# old wrapper\n", encoding="utf-8")
        wrapper.chmod(0o700)
        inittab_sha = hashlib.sha256(self.inittab.read_bytes()).hexdigest()
        initd_sha = hashlib.sha256(self.initd.read_bytes()).hexdigest()
        wrapper_sha = hashlib.sha256(wrapper.read_bytes()).hexdigest()

        res_c = subprocess.run(
            [str(INSTALL_SCRIPT), str(self.app_root)],
            env=self._base_env(999999),
            capture_output=True, text=True, timeout=20,
        )
        self.assertNotEqual(res_c.returncode, 0)
        self.assertIn("版本不一致", res_c.stderr)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), inittab_sha)
        self.assertEqual(hashlib.sha256(self.initd.read_bytes()).hexdigest(), initd_sha)
        self.assertEqual(hashlib.sha256(wrapper.read_bytes()).hexdigest(), wrapper_sha)
        self.assertEqual(list(self.tmp.glob("etc_inittab.bak.*")), [])

    def test_start_refuses_mutating_inittab_when_entry_missing(self):
        (self.app_root / "service.conf").write_text(
            f"executable={self.app_root}/current/smart_gateway\nserial=\nbaud=115200\nserver=127.0.0.1\nport=9000\n",
            encoding="utf-8",
        )
        self.inittab.write_text("id:3:initdefault:\n", encoding="utf-8")
        before = hashlib.sha256(self.inittab.read_bytes()).hexdigest()
        res = self._run_s99(S99_SCRIPT, "start", 999999, expect_rc=1)
        self.assertIn("respawn entry missing", res.stderr)
        self.assertEqual(hashlib.sha256(self.inittab.read_bytes()).hexdigest(), before)


if __name__ == "__main__":
    unittest.main()
