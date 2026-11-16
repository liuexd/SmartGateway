#!/usr/bin/env python3
"""G2-R3 host tests, using owned temporary files and local child processes."""
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / "bsp/apps/boot_ab"
PROBE = BOOT / "supervisor_probe_cli"
AGENT = BOOT / "sgw_system_health"
SLEEP = "/usr/bin/sleep"


def proc_starttime(pid):
    text = Path("/proc/%d/stat" % pid).read_text()
    return int(text[text.rfind(")") + 2:].split()[19])


class SupervisorFixture:
    def __init__(self, mode="ok", external_pid=0):
        # The production probe rejects writable ancestors such as /tmp.
        self.directory = Path(tempfile.mkdtemp(prefix=".sgw-r3-test-", dir=Path.home()))
        self.sock = self.directory / "supervisor.sock"
        ready_r, ready_w = os.pipe()
        self.pid = os.fork()
        if self.pid == 0:
            os.close(ready_r)
            self._serve(mode, external_pid, ready_w)
        os.close(ready_w)
        ready, _, _ = select.select([ready_r], [], [], 5)
        if not ready or os.read(ready_r, 1) != b"1":
            os.close(ready_r)
            self.close()
            raise RuntimeError("test server failed to start")
        os.close(ready_r)

    def _serve(self, mode, external_pid, ready_w):
        child = 0
        signal.signal(signal.SIGTERM, lambda *_: sys.exit(0))
        try:
            if not external_pid:
                child = os.fork()
                if child == 0:
                    os.execv(SLEEP, [SLEEP, "30"])
                pid = child
                deadline = time.monotonic() + 3
                while time.monotonic() < deadline:
                    try:
                        if os.path.samefile("/proc/%d/exe" % pid, SLEEP):
                            break
                    except FileNotFoundError:
                        pass
                    time.sleep(0.01)
            else:
                pid = external_pid
            start = proc_starttime(pid)
            listener = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            listener.bind(str(self.sock))
            os.chmod(self.sock, 0o600)
            listener.listen(1)
            os.write(ready_w, b"1")
            os.close(ready_w)
            conn, _ = listener.accept()
            with conn:
                conn.recv(64)
                if mode == "timeout":
                    time.sleep(1)
                elif mode == "stopped":
                    conn.sendall(b"OK 0 0 stopped\n")
                elif mode == "malformed":
                    conn.sendall(b"OK invalid-starttime\n")
                else:
                    if mode == "stale":
                        start += 1
                    conn.sendall(("OK %d %d running\n" % (pid, start)).encode())
                    time.sleep(0.5)
            listener.close()
        finally:
            signal.signal(signal.SIGTERM, signal.SIG_IGN)
            if child:
                try:
                    os.kill(child, signal.SIGKILL)
                    os.waitpid(child, 0)
                except ProcessLookupError:
                    pass
            os._exit(0)

    def close(self):
        if getattr(self, "pid", 0):
            try:
                os.kill(self.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(self.pid, 0)
            except ChildProcessError:
                pass
            self.pid = 0
        shutil.rmtree(self.directory, ignore_errors=True)


class G2R3Tests(unittest.TestCase):
    def call_probe(self, fixture, expected_gateway=SLEEP, uid=None, timeout=1000,
                   expected_supervisor=sys.executable, socket_path=None):
        return subprocess.run(
            [str(PROBE), str(socket_path or fixture.sock), expected_supervisor, expected_gateway,
             str(os.getuid() if uid is None else uid), str(timeout)],
            capture_output=True, text=True, timeout=5,
        )

    def case(self, mode="ok", **kwargs):
        fixture = SupervisorFixture(mode=mode)
        try:
            return self.call_probe(fixture, **kwargs)
        finally:
            fixture.close()

    def test_actual_supervisor_socket_and_child_identity(self):
        result = self.case()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("managed_pid=", result.stdout)

    def test_wrong_peer_uid_and_wrong_gateway_executable_rejected(self):
        self.assertNotEqual(self.case(uid=os.getuid() + 1).returncode, 0)
        self.assertNotEqual(self.case(expected_gateway="/usr/bin/true").returncode, 0)
        self.assertNotEqual(self.case(expected_supervisor="/usr/bin/true").returncode, 0)

    def test_writable_socket_directory_is_rejected(self):
        fixture = SupervisorFixture()
        try:
            fixture.directory.chmod(0o777)
            self.assertNotEqual(self.call_probe(fixture).returncode, 0)
        finally:
            fixture.directory.chmod(0o700)
            fixture.close()

    def test_symlink_in_untrusted_parent_is_rejected(self):
        fixture = SupervisorFixture()
        unsafe = Path(tempfile.mkdtemp(prefix=".sgw-r3-unsafe-", dir=Path.home()))
        try:
            unsafe.chmod(0o777)
            alias = unsafe / "supervisor.sock"
            alias.symlink_to(fixture.sock)
            self.assertNotEqual(self.call_probe(fixture, socket_path=alias).returncode, 0)
        finally:
            shutil.rmtree(unsafe, ignore_errors=True)
            fixture.close()

    def test_stale_pid_stopped_and_timeout_rejected(self):
        self.assertNotEqual(self.case("stale").returncode, 0)
        self.assertNotEqual(self.case("stopped").returncode, 0)
        self.assertNotEqual(self.case("malformed").returncode, 0)
        self.assertNotEqual(self.case("timeout", timeout=150).returncode, 0)

    def test_gateway_not_owned_by_supervisor_rejected(self):
        other = subprocess.Popen([SLEEP, "30"])
        fixture = SupervisorFixture(external_pid=other.pid)
        try:
            self.assertNotEqual(self.call_probe(fixture).returncode, 0)
        finally:
            fixture.close()
            other.terminate()
            other.wait(timeout=3)

    def test_diagnostic_never_confirms_or_trusts_cli_health_flags(self):
        result = subprocess.run([str(AGENT), "--once"], capture_output=True, text=True, timeout=5)
        self.assertEqual(result.returncode, 3)
        self.assertTrue("overall=UNKNOWN" in result.stdout or
                        "overall=UNHEALTHY" in result.stdout)
        self.assertIn("confirmation=DISABLED", result.stdout)
        for item in ("gateway_business", "local_network", "data"):
            self.assertIn(item + "=UNKNOWN", result.stdout)
        for item in ("gpio", "ap3216_i2c"):
            self.assertTrue(any(item + "=" + value in result.stdout
                                for value in ("HEALTHY", "UNHEALTHY", "UNKNOWN")))
        result = subprocess.run([str(AGENT), "--health-ok", "1"], capture_output=True, text=True)
        self.assertNotEqual(result.returncode, 0)


if __name__ == "__main__":
    unittest.main()
