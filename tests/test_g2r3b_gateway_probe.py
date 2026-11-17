#!/usr/bin/env python3
"""Non-invasive local Gateway probe against an isolated real Supervisor/Gateway."""
import os
import json
from pathlib import Path
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / "bsp/apps/boot_ab"
SUP_BIN = ROOT / "bsp/apps/service_supervisor/service_supervisor"
GW_BIN = ROOT / "build/r3b-gateway/smart_gateway"
SUP_PROBE = BOOT / "supervisor_probe_cli"
GW_PROBE = BOOT / "gateway_probe_cli"


def proc_start(pid):
    line = Path("/proc/%d/stat" % pid).read_text()
    return int(line[line.rfind(")") + 2:].split()[19])


class GatewayProbeTests(unittest.TestCase):
    def setUp(self):
        # Never disturb another Gateway that happens to own the fixed test port.
        check = socket.socket()
        try:
            check.bind(("0.0.0.0", 7000))
        except OSError as exc:
            check.close()
            self.skipTest("Gateway port 7000 unavailable: %s" % exc)
        check.close()
        self.tmp = Path(tempfile.mkdtemp(prefix=".sgw-r3b-", dir=Path.home()))
        self.addCleanup(self.cleanup_fixture)
        (self.tmp / "run").mkdir(mode=0o700)
        (self.tmp / "bin").mkdir()
        (self.tmp / "releases/v1").mkdir(parents=True)
        for directory in ("bin", "releases", "releases/v1"):
            (self.tmp / directory).chmod(0o755)
        self.sup_exe = self.tmp / "bin/service_supervisor"
        self.gateway_exe = self.tmp / "releases/v1/smart_gateway"
        shutil.copy2(SUP_BIN, self.sup_exe)
        shutil.copy2(GW_BIN, self.gateway_exe)
        self.sup_exe.chmod(0o755)
        self.gateway_exe.chmod(0o755)
        (self.tmp / "current").symlink_to("releases/v1", target_is_directory=True)
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        port = self.server.getsockname()[1]
        (self.tmp / "service.conf").write_text(
            "executable=%s/current/smart_gateway\n"
            "serial=\nserver=127.0.0.1\nport=%d\nstop_timeout_ms=1000\n" %
            (self.tmp, port), encoding="utf-8")
        self.log = (self.tmp / "supervisor.log").open("wb")
        env = os.environ.copy()
        # Accepted only in the separate test build, absent from production.
        env["SGW_GATEWAY_HEALTH_TEST_DIR"] = str(self.tmp / "run")
        self.sup = subprocess.Popen([str(self.sup_exe), "run", "--root", str(self.tmp)],
                                    env=env, stdout=self.log, stderr=subprocess.STDOUT)
        self.gateway_pid = 0
        self.stopped = False
        sup_sock = self.tmp / "run/supervisor.sock"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not sup_sock.exists():
            if self.sup.poll() is not None:
                raise RuntimeError("isolated Supervisor exited")
            time.sleep(0.02)
        result = subprocess.run([str(self.sup_exe), "start", "--root", str(self.tmp)],
                                capture_output=True, text=True, timeout=8)
        if result.returncode != 0:
            raise RuntimeError("isolated Gateway start failed: " + result.stdout + result.stderr)
        self.server.settimeout(5)
        self.upstream, _ = self.server.accept()
        self.health_sock = self.tmp / "run/gateway-health.sock"
        while time.monotonic() < deadline and not self.health_sock.exists():
            time.sleep(0.02)
        sup = subprocess.run([str(SUP_PROBE), str(sup_sock), str(self.sup_exe),
                              str(self.tmp / "current/smart_gateway"),
                              str(os.getuid()), "1500"],
                             capture_output=True, text=True, timeout=5)
        if sup.returncode != 0:
            raise RuntimeError("Supervisor identity check failed: " + sup.stdout + sup.stderr)
        self.gateway_pid = int(sup.stdout.strip().split("=")[1])
        self.starttime = proc_start(self.gateway_pid)

    def cleanup_fixture(self):
        if getattr(self, "stopped", False) and getattr(self, "gateway_pid", 0):
            try: os.kill(self.gateway_pid, signal.SIGCONT)
            except ProcessLookupError: pass
        if getattr(self, "sup", None) and self.sup.poll() is None:
            try:
                subprocess.run([str(self.sup_exe), "stop", "--root", str(self.tmp)],
                               capture_output=True, timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass
            self.sup.terminate()
            try: self.sup.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.sup.kill()
                self.sup.wait(timeout=3)
        if getattr(self, "upstream", None): self.upstream.close()
        if getattr(self, "server", None): self.server.close()
        if getattr(self, "log", None): self.log.close()
        if getattr(self, "tmp", None): shutil.rmtree(self.tmp, ignore_errors=True)

    def probe(self, pid=None, start=None, timeout=800, flow=False, local=False):
        args = [str(GW_PROBE), str(self.health_sock), str(os.getuid()),
                str(self.gateway_pid if pid is None else pid),
                str(self.starttime if start is None else start), str(timeout)]
        if local:
            args.append("l")
        elif flow:
            args.append("f")
        return subprocess.run(args, capture_output=True, text=True, timeout=5)

    def test_actual_gateway_components_and_no_upstream_pollution(self):
        result = self.probe()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("components-ready", result.stdout)
        self.assertNotEqual(self.probe(flow=True).returncode, 0)
        self.upstream.settimeout(0.3)
        with self.assertRaises(socket.timeout):
            self.upstream.recv(4096)
        self.assertEqual(self.probe(start=self.starttime + 1).returncode, 1)
        # Existing DATA protocol still reaches the real upstream unchanged.
        payload = b'{"node":"NODE01","seq":17,"type":"DATA","fields":{"T":"253","H":"601"}}\n'
        with socket.create_connection(("127.0.0.1", 7000), timeout=2) as node:
            node.sendall(payload)
        self.upstream.settimeout(3)
        received = b""
        while b"\n" not in received:
            chunk = self.upstream.recv(1024)
            self.assertTrue(chunk)
            received += chunk
        data = json.loads(received.split(b"\n", 1)[0])
        self.assertEqual(data["node"], "NODE01")
        self.assertEqual(data["seq"], 17)
        self.assertEqual(data["fields"], {"T": "253", "H": "601"})
        self.assertEqual(self.probe(flow=True).returncode, 0)
        # No probe traffic is sent upstream. Idle/stale evidence is UNKNOWN.
        self.upstream.settimeout(0.3)
        with self.assertRaises(socket.timeout):
            self.upstream.recv(4096)
        time.sleep(6.1)
        self.assertNotEqual(self.probe(flow=True).returncode, 0)

    def test_local_probe_with_disconnected_upstream_preserves_pending_data(self):
        port = self.server.getsockname()[1]
        self.upstream.shutdown(socket.SHUT_RDWR)
        self.upstream.close()
        self.upstream = None
        self.server.close()
        self.server = None
        time.sleep(0.5)
        payload = b'{"node":"NODE01","seq":63,"type":"DATA","fields":{"T":"253"}}\n'
        with socket.create_connection(("127.0.0.1", 7000), timeout=2) as node:
            node.sendall(payload)
        time.sleep(0.5)
        result = self.probe(local=True, timeout=1500)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("local-observed", result.stdout)
        self.server = socket.socket()
        self.server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.server.bind(("127.0.0.1", port))
        self.server.listen(1)
        self.server.settimeout(8)
        self.upstream, _ = self.server.accept()
        self.upstream.settimeout(8)
        received = b""
        while b"\n" not in received:
            chunk = self.upstream.recv(1024)
            self.assertTrue(chunk)
            received += chunk
        data = json.loads(received.split(b"\n", 1)[0])
        self.assertEqual(data["seq"], 63)
        self.upstream.settimeout(0.3)
        with self.assertRaises(socket.timeout):
            self.upstream.recv(4096)

    def test_live_pid_but_unresponsive_gateway_is_rejected(self):
        os.kill(self.gateway_pid, signal.SIGSTOP)
        self.stopped = True
        result = self.probe(timeout=250)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(Path("/proc/%d" % self.gateway_pid).exists())


if __name__ == "__main__":
    unittest.main()
