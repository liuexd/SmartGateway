#!/usr/bin/env python3
"""G2-R3C N09/N10 end-to-end integration tests in isolated host fixtures.

Exercises the real production coordinator (`r3c_coordinator_confirm`), real
`service_supervisor`, real `smart_gateway` (SGW-PROBE/1, SGW-FLOWX/1, SGW-LOCAL/1),
real `fuse2fs` ext4 DATA image, real RSA/SHA-256 bundle -> write/readback -> pending ->
boot_id binding -> stable observation -> confirm + consumed anti-replay -> fsync fault.
Never modifies real board eMMC, U-Boot env, or production directories.
"""
import fcntl
import hashlib
import json
import os
from pathlib import Path
import select
import shutil
import signal
import socket
import subprocess
import tempfile
import time
import unittest
import uuid

ROOT = Path(__file__).resolve().parents[1]
BOOT = ROOT / "bsp/apps/boot_ab"
HARNESS = BOOT / "r3c_confirm_harness"
SUP_BIN = ROOT / "bsp/apps/service_supervisor/service_supervisor"
GW_BIN = ROOT / "build/r3b-gateway/smart_gateway"
UBOOT_BUILD = Path(os.environ.get("UBOOT_AB_BUILD_DIR", str(ROOT / "build/uboot-ab")))
HOST_FW_PRINTENV = UBOOT_BUILD / "host/fw_printenv"
HOST_FW_SETENV = UBOOT_BUILD / "host/fw_setenv"
FSYNC_FAIL_C = ROOT / "tests/r3c_fsync_fail.c"
ENV_OFFSET_0 = 0xE0000
ENV_OFFSET_1 = 0xE2000
ENV_SIZE = 0x2000


def find_fuse2fs():
    env = os.environ.get("SGW_TEST_FUSE2FS")
    if env and Path(env).exists():
        return env
    w = shutil.which("fuse2fs")
    if w:
        return w
    fallback = Path("/tmp/sgw-r3c2-rOMqb4/fuse-pkg/usr/bin/fuse2fs")
    if fallback.exists():
        return str(fallback)
    return None


FUSE2FS = find_fuse2fs()


def parse_env(text):
    kv = {}
    for line in text.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            kv[k] = v
    return kv


@unittest.skipUnless(FUSE2FS, "fuse2fs binary required for isolated ext4 DATA mount")
class TestG2R3CN09N10(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (HARNESS, SUP_BIN, GW_BIN, HOST_FW_PRINTENV, HOST_FW_SETENV):
            if not p.exists():
                raise RuntimeError(f"Missing required artifact: {p}")

    def setUp(self):
        chk = socket.socket()
        try:
            chk.bind(("0.0.0.0", 7000))
        except OSError as exc:
            chk.close()
            self.skipTest(f"Gateway TCP port 7000 busy: {exc}")
        chk.close()

        # Trusted parent dir under HOME (chmod 0700 so trusted_dirs passes)
        self.tmp = tempfile.TemporaryDirectory(prefix=".sgw-n09-n10-", dir=Path.home())
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(self.cleanup_all)
        self.base = Path(self.tmp.name)
        self.base.chmod(0o700)

        # 1. Redundant U-Boot env binary image
        self.emmc_img = self.base / "emmc.img"
        with self.emmc_img.open("wb") as f:
            f.truncate(4 * 1024 * 1024)
        self.fw_cfg = self.base / "fw_env.config"
        self.fw_cfg.write_text(
            f"{self.emmc_img} 0x{ENV_OFFSET_0:x} 0x{ENV_SIZE:x}\n"
            f"{self.emmc_img} 0x{ENV_OFFSET_1:x} 0x{ENV_SIZE:x}\n",
            encoding="utf-8",
        )
        self.set_env({
            "sgw_active_slot": "2",
            "sgw_last_good_slot": "2",
            "sgw_pending_slot": "none",
            "sgw_ab_state": "confirmed",
            "bootlimit": "3",
            "bootcount": "0",
            "upgrade_available": "0",
            "sgw_rollback_count": "0",
            "sgw_last_error": "none",
        })

        # 2. Isolated OS context (cmdline, mounts, sysroot, boot_id, fake slot image)
        self.sysroot = self.base / "sysroot"
        (self.sysroot / "sbin").mkdir(parents=True)
        (self.sysroot / "etc").mkdir(parents=True)
        (self.sysroot / "sbin/init").write_text("init\n")
        (self.sysroot / "etc/inittab").write_text("::sysinit:/etc/init.d/rcS\n")
        self.cmdline = self.base / "cmdline"
        self.mounts = self.base / "mounts"
        self.set_running_slot("2")
        self.boot_id_file = self.base / "boot_id"
        self.boot_id = str(uuid.uuid4())
        self.boot_id_file.write_text(self.boot_id + "\n", encoding="utf-8")
        self.slot1_img = self.base / "slot1.ext4"

        # 3. FUSE-mounted isolated ext4 DATA partition + schema=1 + tx_dir
        self.data_img = self.base / "data.ext4"
        self.data_mnt = self.base / "data"
        self.data_mnt.mkdir(mode=0o700)
        self.mounted = False
        with self.data_img.open("wb") as f:
            f.truncate(32 * 1024 * 1024)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", str(self.data_img)], check=True)
        uid, gid = os.getuid(), os.getgid()
        for key, val in (("uid", uid), ("gid", gid)):
            subprocess.run(["debugfs", "-w", "-R", f"set_inode_field <2> {key} {val}",
                            str(self.data_img)], check=True, capture_output=True)
        subprocess.run([FUSE2FS, str(self.data_img), str(self.data_mnt)],
                       check=True, capture_output=True, timeout=10)
        self.mounted = True
        self.data_mnt.chmod(0o700)
        schema = self.data_mnt / ".sgw-data-schema"
        schema.write_text("schema=1\n", encoding="utf-8")
        schema.chmod(0o600)
        self.txdir = self.data_mnt / ".sgw-tx"
        self.txdir.mkdir(mode=0o700)

        # 4. Isolated App OTA settled tree (.ota.lock + state + current -> releases/v1)
        self.app_dir = self.base / "app_ota"
        self.app_dir.mkdir(mode=0o755)
        self.app_dir.chmod(0o755)
        (self.app_dir / "run").mkdir(mode=0o700)
        (self.app_dir / "run").chmod(0o700)
        (self.app_dir / "bin").mkdir(mode=0o755)
        (self.app_dir / "bin").chmod(0o755)
        (self.app_dir / "state").mkdir(mode=0o700)
        (self.app_dir / "state").chmod(0o700)
        (self.app_dir / "releases").mkdir(mode=0o755)
        (self.app_dir / "releases").chmod(0o755)
        (self.app_dir / "releases/v1").mkdir(mode=0o755)
        (self.app_dir / "releases/v1").chmod(0o755)
        self.sup_exe = self.app_dir / "bin/service_supervisor"
        self.gw_exe = self.app_dir / "releases/v1/smart_gateway"
        shutil.copy2(SUP_BIN, self.sup_exe)
        shutil.copy2(GW_BIN, self.gw_exe)
        self.sup_exe.chmod(0o755)
        self.gw_exe.chmod(0o755)
        (self.app_dir / "current").symlink_to("releases/v1", target_is_directory=True)
        self.ota_lock = self.app_dir / ".ota.lock"
        self.ota_lock.write_text("", encoding="utf-8")
        self.ota_lock.chmod(0o600)
        (self.app_dir / "state/ota_state.json").write_text(
            '{\n  "schema_version": 1,\n  "state": "CONFIRMED",\n'
            '  "confirmed_version": "v1",\n  "candidate_version": "v2"\n}\n',
            encoding="utf-8",
        )
        (self.app_dir / "state/ota_state.json").chmod(0o600)

        # 5. Isolated RSA-2048 signed system bundle
        self.bundle = self.base / "bundle"
        self.bundle.mkdir(mode=0o700)
        self.privkey = self.base / "priv.pem"
        self.pubkey = self.base / "pub.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                        "rsa_keygen_bits:2048", "-out", str(self.privkey)],
                       check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(self.privkey), "-pubout",
                        "-out", str(self.pubkey)], check=True, capture_output=True)
        self.pubkey.chmod(0o600)
        img_bytes = b"G3-TRIMMED-ROOTFS-TEST-PAYLOAD\x00" * 512
        (self.bundle / "rootfs.img").write_bytes(img_bytes)
        manifest = ("SGW-SYSTEM-1\nboard=imx6ull-100ask\nimage=rootfs.img\n"
                    f"size={len(img_bytes)}\nsha256={hashlib.sha256(img_bytes).hexdigest()}\n")
        (self.bundle / "manifest").write_text(manifest, encoding="utf-8")
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(self.privkey),
                        "-out", str(self.bundle / "manifest.sig"),
                        str(self.bundle / "manifest")], check=True, capture_output=True)
        for p in self.bundle.iterdir():
            p.chmod(0o600)

        # 6. Isolated GPIO & AP3216C sysfs fixtures
        drv_base = self.base / "drivers"
        (drv_base / "sgw-gpio-event").mkdir(parents=True)
        (drv_base / "sgw_ap3216").mkdir(parents=True)
        self.gpio_dir = self.base / "gpio"
        self.gpio_dir.mkdir(mode=0o700)
        (self.gpio_dir / "driver").symlink_to(drv_base / "sgw-gpio-event")
        (self.gpio_dir / "misc_dev").write_text("1:3\n", encoding="utf-8")
        (self.gpio_dir / "stats").write_text(
            "irq_total=0\nenqueued=0\ndropped=0\ndepth=0\ncapacity=16\n"
            "head=0\ntail=0\nlast_sequence=0\nlast_level=0\nlast_timestamp_ns=0\n",
            encoding="utf-8",
        )
        self.ap_dir = self.base / "ap3216"
        self.ap_dir.mkdir(mode=0o700)
        (self.ap_dir / "driver").symlink_to(drv_base / "sgw_ap3216")
        (self.ap_dir / "als_raw").write_text("0\n", encoding="utf-8")
        (self.ap_dir / "stats").write_text("read_success=3\nread_errors=0\n", encoding="utf-8")

        # 7. Real Supervisor + real smart_gateway + mock upstream server
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        port = self.server.getsockname()[1]
        (self.app_dir / "service.conf").write_text(
            f"executable={self.gw_exe}\nserial=\nserver=127.0.0.1\nport={port}\nstop_timeout_ms=1000\n",
            encoding="utf-8",
        )
        self.sup_log = (self.base / "supervisor.log").open("wb")
        senv = os.environ.copy()
        senv["SGW_GATEWAY_HEALTH_TEST_DIR"] = str(self.app_dir / "run")
        self.sup = subprocess.Popen([str(self.sup_exe), "run", "--root", str(self.app_dir)],
                                    env=senv, stdout=self.sup_log, stderr=subprocess.STDOUT)
        sup_sock = self.app_dir / "run/supervisor.sock"
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline and not sup_sock.exists():
            if self.sup.poll() is not None:
                raise RuntimeError("Supervisor exited early")
            time.sleep(0.02)
        res = subprocess.run([str(self.sup_exe), "start", "--root", str(self.app_dir)],
                             capture_output=True, text=True, timeout=8)
        if res.returncode != 0:
            raise RuntimeError("Supervisor start failed: " + res.stdout + res.stderr)
        self.server.settimeout(5)
        self.upstream, _ = self.server.accept()
        self.gw_sock = self.app_dir / "run/gateway-health.sock"
        while time.monotonic() < deadline and not self.gw_sock.exists():
            time.sleep(0.02)
        self.policy_file = self.base / "r3c-policy.conf"

    def cleanup_all(self):
        if getattr(self, "sup", None) and self.sup.poll() is None:
            try:
                subprocess.run([str(self.sup_exe), "stop", "--root", str(self.app_dir)],
                               capture_output=True, timeout=4)
            except (OSError, subprocess.TimeoutExpired):
                pass
            self.sup.terminate()
            try:
                self.sup.wait(timeout=4)
            except subprocess.TimeoutExpired:
                self.sup.kill()
                self.sup.wait(timeout=3)
        for sock_attr in ("upstream", "server"):
            s = getattr(self, sock_attr, None)
            if s:
                try:
                    s.close()
                except OSError:
                    pass
        if getattr(self, "sup_log", None):
            self.sup_log.close()
        if getattr(self, "mounted", False):
            subprocess.run(["fusermount3", "-u", str(self.data_mnt)],
                           capture_output=True, timeout=10)
            self.mounted = False

    def set_env(self, pairs):
        script = "".join(f"{k} {v}\n" for k, v in pairs.items())
        subprocess.run([str(HOST_FW_SETENV), "-c", str(self.fw_cfg), "-s", "-"],
                       input=script, capture_output=True, text=True, check=True)

    def get_env(self):
        r = subprocess.run([str(HOST_FW_PRINTENV), "-c", str(self.fw_cfg)],
                           capture_output=True, text=True, check=True)
        return parse_env(r.stdout)

    def set_running_slot(self, slot):
        self.cmdline.write_text(f"console=ttymxc0,115200 root=/dev/mmcblk1p{slot} rootwait rw\n")
        self.mounts.write_text("/dev/root / ext4 rw,relatime 0 0\n")

    def write_policy(self, flow_mode="strict_forward", allow_degraded="0",
                     stable_ms="150", max_gap_ms="400", deadline_ms="2000",
                     max_age_ms="2000", req_hb="1"):
        self.policy_file.write_text(
            f"data_mountpoint={self.data_mnt}\n"
            f"data_device=/dev/mmcblk1p3\n"
            f"data_min_bytes=1048576\n"
            f"app_ota_dir={self.app_dir}\n"
            f"tx_dir={self.txdir}\n"
            f"pubkey_path={self.pubkey}\n"
            f"supervisor_sock={self.app_dir / 'run/supervisor.sock'}\n"
            f"supervisor_exe={self.sup_exe}\n"
            f"gateway_sock={self.gw_sock}\n"
            f"gateway_exe={self.gw_exe}\n"
            f"gateway_flow_mode={flow_mode}\n"
            f"allow_external_degraded={allow_degraded}\n"
            f"require_sensor_heartbeat={req_hb}\n"
            f"stable_ms={stable_ms}\n"
            f"max_gap_ms={max_gap_ms}\n"
            f"deadline_ms={deadline_ms}\n"
            f"max_evidence_age_ms={max_age_ms}\n",
            encoding="utf-8",
        )
        self.policy_file.chmod(0o600)

    def harness_env(self, preload=None):
        env = os.environ.copy()
        env["SGW_FW_PRINTENV"] = str(HOST_FW_PRINTENV)
        env["SGW_FW_SETENV"] = str(HOST_FW_SETENV)
        env["SGW_FW_ENV_CONFIG"] = str(self.fw_cfg)
        if preload:
            env["LD_PRELOAD"] = str(preload)
        return env

    def stage_signed_upgrade_to_slot1(self):
        r = subprocess.run([
            str(HARNESS), "write-and-stage",
            "--txdir", str(self.txdir),
            "--bundle", str(self.bundle),
            "--pubkey", str(self.pubkey),
            "--slot-img", str(self.slot1_img),
            "--fw-cfg", str(self.fw_cfg),
            "--cmdline", str(self.cmdline),
            "--mounts", str(self.mounts),
            "--mountinfo", "/proc/self/mountinfo",
            "--sysroot", str(self.sysroot),
            "--target", "1",
            "--last-good", "2",
        ], env=self.harness_env(), capture_output=True, text=True, timeout=15)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        txid = r.stdout.strip().split("tx=")[1].split()[0]
        # Simulate U-Boot booting trial slot 1 with bootcount=1
        self.set_env({"sgw_active_slot": "1", "sgw_ab_state": "trial", "bootcount": "1"})
        self.set_running_slot("1")
        return txid

    def run_confirm(self, net="healthy", hb="healthy", preload=None, running=None, extra_env=None):
        cmd = [
            str(HARNESS), "confirm",
            "--policy", str(self.policy_file),
            "--fw-cfg", str(self.fw_cfg),
            "--cmdline", str(self.cmdline),
            "--mounts", str(self.mounts),
            "--mountinfo", "/proc/self/mountinfo",
            "--sysroot", str(self.sysroot),
            "--slot-img", str(self.slot1_img),
            "--boot-id-file", str(self.boot_id_file),
            "--gpio-dir", str(self.gpio_dir),
            "--ap3216-dir", str(self.ap_dir),
            "--net-state", net,
            "--sensor-hb", hb,
            "--fuse-data",
        ]
        if running is not None:
            cmd.extend(["--running", str(running)])
        env = self.harness_env(preload=preload)
        if extra_env:
            env.update(extra_env)
        return subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=15)

    def send_real_data_frame(self, seq=42):
        payload = json.dumps({
            "node": "NODE01", "seq": seq, "type": "DATA",
            "fields": {"T": "251", "H": "590"},
        }).encode("utf-8") + b"\n"
        with socket.create_connection(("127.0.0.1", 7000), timeout=2) as node:
            node.sendall(payload)
        self.upstream.settimeout(3)
        buf = b""
        while b"\n" not in buf:
            chunk = self.upstream.recv(1024)
            self.assertTrue(chunk)
            buf += chunk
        msg = json.loads(buf.split(b"\n", 1)[0])
        self.assertEqual(msg["seq"], seq)

    def inspect_txn(self, txid):
        r = subprocess.run([
            str(HARNESS), "inspect-txn",
            "--txdir", str(self.txdir), "--id", txid,
        ], capture_output=True, text=True, check=True)
        return r.stdout.strip()

    def test_n09_strict_forward_end_to_end_confirm_and_n10_replay_rejected(self):
        txid = self.stage_signed_upgrade_to_slot1()
        self.write_policy(flow_mode="strict_forward", allow_degraded="0")
        # Before real DATA flow, strict_forward must NOT confirm
        r_no_flow = self.run_confirm()
        self.assertNotEqual(r_no_flow.returncode, 0)
        self.assertEqual(self.get_env()["sgw_ab_state"], "trial")

        # Send real DATA packet -> forwarded to mock upstream -> SGW-FLOWX/1 FORWARDED
        self.send_real_data_frame(seq=101)
        r_ok = self.run_confirm()
        self.assertEqual(r_ok.returncode, 0, r_ok.stdout + r_ok.stderr)
        self.assertIn("CONFIRMED slot=1 last_good=1", r_ok.stdout)
        self.assertIn("state=HEALTHY", r_ok.stdout)

        # Ensure zero extra/synthetic probe packets polluted mock upstream
        self.upstream.settimeout(0.2)
        with self.assertRaises(socket.timeout):
            self.upstream.recv(4096)

        ev = self.get_env()
        self.assertEqual(ev["sgw_active_slot"], "1")
        self.assertEqual(ev["sgw_last_good_slot"], "1")
        self.assertEqual(ev["sgw_pending_slot"], "none")
        self.assertEqual(ev["sgw_ab_state"], "confirmed")
        self.assertEqual(ev["upgrade_available"], "0")
        self.assertEqual(ev["bootcount"], "0")

        txn_info = self.inspect_txn(txid)
        self.assertIn("state=CONFIRMED consumed=1", txn_info)
        self.assertIn(f"boot_id={self.boot_id}", txn_info)

        # N10 Replay: even if U-Boot env is forced back to trial, consumed txn cannot confirm again
        self.set_env({
            "sgw_active_slot": "1", "sgw_last_good_slot": "2",
            "sgw_pending_slot": "1", "sgw_ab_state": "trial",
            "upgrade_available": "1", "bootcount": "2",
        })
        self.send_real_data_frame(seq=102)
        r_replay = self.run_confirm()
        self.assertNotEqual(r_replay.returncode, 0)
        self.assertEqual(self.get_env()["sgw_ab_state"], "trial")

    def test_n09_controlled_local_policy_decoupling_without_upstream_traffic(self):
        txid = self.stage_signed_upgrade_to_slot1()
        # No DATA forwarded at all; server_link worker is ticking cleanly (SGW-LOCAL/1 LOCAL_OK)
        self.write_policy(flow_mode="controlled_local", allow_degraded="0")
        r_deny = self.run_confirm()
        self.assertNotEqual(r_deny.returncode, 0)
        self.assertEqual(self.get_env()["sgw_ab_state"], "trial")

        # Enable policy tolerance for external DEGRADED -> confirm succeeds cleanly
        self.write_policy(flow_mode="controlled_local", allow_degraded="1")
        r_allow = self.run_confirm()
        self.assertEqual(r_allow.returncode, 0, r_allow.stdout + r_allow.stderr)
        self.assertIn("state=DEGRADED", r_allow.stdout)
        self.assertIn("state=CONFIRMED consumed=1", self.inspect_txn(txid))
        self.assertEqual(self.get_env()["sgw_ab_state"], "confirmed")

    def test_n10_tamper_lock_contention_device_and_policy_faults_rejected(self):
        txid = self.stage_signed_upgrade_to_slot1()
        self.write_policy(flow_mode="controlled_local", allow_degraded="1")

        # 1. Untrusted policy permissions rejected
        self.policy_file.chmod(0o666)
        r_pol = self.run_confirm()
        self.assertNotEqual(r_pol.returncode, 0)
        self.assertIn("untrusted policy", r_pol.stderr)
        self.policy_file.chmod(0o600)

        # 2. App OTA exclusive lock held blocks confirm
        with self.ota_lock.open("r+") as lfp:
            fcntl.flock(lfp.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            r_lock = self.run_confirm()
            self.assertNotEqual(r_lock.returncode, 0)
            self.assertIn("application OTA unavailable", r_lock.stderr)
            fcntl.flock(lfp.fileno(), fcntl.LOCK_UN)

        # 3. Unhealthy sensor heartbeat or network rejected
        r_hb = self.run_confirm(hb="unhealthy")
        self.assertNotEqual(r_hb.returncode, 0)
        self.assertIn("UNHEALTHY", r_hb.stderr)
        r_net = self.run_confirm(net="unhealthy")
        self.assertNotEqual(r_net.returncode, 0)
        self.assertIn("UNHEALTHY", r_net.stderr)

        # 4. Tampered installed slot image rejected
        orig = self.slot1_img.read_bytes()
        self.slot1_img.write_bytes(b"CORRUPTED" + orig[9:])
        r_tamper = self.run_confirm()
        self.assertNotEqual(r_tamper.returncode, 0)
        self.assertIn("digest mismatch", r_tamper.stderr)
        self.slot1_img.write_bytes(orig)

        # 5. Gateway stopped blocks confirm
        subprocess.run([str(self.sup_exe), "stop", "--root", str(self.app_dir)],
                       capture_output=True, timeout=5)
        r_gw = self.run_confirm()
        self.assertNotEqual(r_gw.returncode, 0)
        self.assertIn("health observation", r_gw.stderr)
        self.assertIn("state=PENDING consumed=0", self.inspect_txn(txid))
        self.assertEqual(self.get_env()["sgw_ab_state"], "trial")

    def test_n10_persistence_fsync_failure_rejected(self):
        txid = self.stage_signed_upgrade_to_slot1()
        self.write_policy(flow_mode="controlled_local", allow_degraded="1")
        so_path = self.base / "libr3c_fsync_fail.so"
        subprocess.run(["gcc", "-shared", "-fPIC", str(FSYNC_FAIL_C), "-o", str(so_path)], check=True)
        r = self.run_confirm(preload=so_path)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("state=PENDING consumed=0", self.inspect_txn(txid))
        self.assertEqual(self.get_env()["sgw_ab_state"], "trial")

        # Targeted fsync failure specifically on .txn- transaction commit during confirm
        txn_fail_c = self.base / "txn_fsync_fail.c"
        txn_fail_so = self.base / "libtxn_fsync_fail.so"
        txn_fail_c.write_text(
            '#define _GNU_SOURCE\n'
            '#include <dlfcn.h>\n#include <errno.h>\n#include <stdio.h>\n#include <string.h>\n#include <unistd.h>\n'
            'int fsync(int fd) {\n'
            '    char lnk[64], target[512];\n'
            '    int (*real_fsync)(int) = dlsym(RTLD_NEXT, "fsync");\n'
            '    snprintf(lnk, sizeof(lnk), "/proc/self/fd/%d", fd);\n'
            '    ssize_t n = readlink(lnk, target, sizeof(target) - 1);\n'
            '    if (n > 0) { target[n] = 0; if (strstr(target, ".txn-")) { errno = EIO; return -1; } }\n'
            '    return real_fsync(fd);\n'
            '}\n',
            encoding="utf-8",
        )
        subprocess.run(["gcc", "-shared", "-fPIC", str(txn_fail_c), "-ldl", "-o", str(txn_fail_so)], check=True)
        r2 = self.run_confirm(preload=txn_fail_so)
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("state=PENDING consumed=0", self.inspect_txn(txid))
        self.assertEqual(self.get_env()["sgw_ab_state"], "trial")

    def test_n10_caller_running_mismatch_rejects_before_any_mutation(self):
        txid = self.stage_signed_upgrade_to_slot1()
        self.write_policy(flow_mode="strict_forward", allow_degraded="0")
        self.send_real_data_frame(seq=201)

        before_env = self.get_env()
        r_bad = self.run_confirm(running="2")
        self.assertNotEqual(r_bad.returncode, 0)
        self.assertIn("caller --running disagrees", r_bad.stderr)
        # Verify ZERO mutation occurred on either fw_env or transaction ledger
        self.assertEqual(self.get_env(), before_env)
        self.assertIn("state=PENDING consumed=0", self.inspect_txn(txid))

        # Now matching --running 1 confirms cleanly
        self.send_real_data_frame(seq=202)
        r_good = self.run_confirm(running="1")
        self.assertEqual(r_good.returncode, 0, r_good.stdout + r_good.stderr)
        self.assertIn("CONFIRMED slot=1 last_good=1", r_good.stdout)
        self.assertIn("state=CONFIRMED consumed=1", self.inspect_txn(txid))

    def test_n10_split_state_env_confirmed_txn_confirming_recovery_and_idempotency(self):
        txid = self.stage_signed_upgrade_to_slot1()
        self.write_policy(flow_mode="controlled_local", allow_degraded="1")

        # Fault injector: allow 1st .txn- fsync (CONFIRMING), fail 2nd .txn- fsync (CONFIRMED) AFTER fw_env committed!
        split_c = self.base / "split_fsync_fail.c"
        split_so = self.base / "libsplit_fsync_fail.so"
        split_c.write_text(
            '#define _GNU_SOURCE\n'
            '#include <dlfcn.h>\n#include <errno.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n#include <unistd.h>\n'
            'int fsync(int fd) {\n'
            '    static int txn_fsyncs = 0;\n'
            '    char lnk[64], target[512];\n'
            '    int (*real_fsync)(int) = dlsym(RTLD_NEXT, "fsync");\n'
            '    snprintf(lnk, sizeof(lnk), "/proc/self/fd/%d", fd);\n'
            '    ssize_t n = readlink(lnk, target, sizeof(target) - 1);\n'
            '    if (n > 0) {\n'
            '        target[n] = 0;\n'
            '        if (strstr(target, ".txn-")) {\n'
            '            txn_fsyncs++;\n'
            '            if (txn_fsyncs >= 2) { errno = EIO; return -1; }\n'
            '        }\n'
            '    }\n'
            '    return real_fsync(fd);\n'
            '}\n',
            encoding="utf-8",
        )
        subprocess.run(["gcc", "-shared", "-fPIC", str(split_c), "-ldl", "-o", str(split_so)], check=True)

        r_fail = self.run_confirm(preload=split_so)
        self.assertNotEqual(r_fail.returncode, 0)
        # Verify exact split state on disk: fw_env is CONFIRMED, txn is still CONFIRMING (consumed=0)
        ev = self.get_env()
        self.assertEqual(ev["sgw_ab_state"], "confirmed")
        self.assertEqual(ev["upgrade_available"], "0")
        self.assertEqual(ev["bootcount"], "0")
        self.assertEqual(ev["sgw_active_slot"], "1")
        self.assertIn("state=CONFIRMING consumed=0", self.inspect_txn(txid))

        # Simulate reboot into new boot_id
        self.boot_id = str(uuid.uuid4())
        self.boot_id_file.write_text(self.boot_id + "\n", encoding="utf-8")

        # Tampered slot must STILL block split-state recovery
        orig = self.slot1_img.read_bytes()
        self.slot1_img.write_bytes(b"BADSLOT" + orig[7:])
        r_bad_rec = self.run_confirm()
        self.assertNotEqual(r_bad_rec.returncode, 0)
        self.assertIn("digest mismatch", r_bad_rec.stderr)
        self.assertIn("state=CONFIRMING consumed=0", self.inspect_txn(txid))
        self.slot1_img.write_bytes(orig)

        # Clean retry reconciles CONFIRMING -> CONFIRMED (consumed=1)
        r_rec = self.run_confirm()
        self.assertEqual(r_rec.returncode, 0, r_rec.stdout + r_rec.stderr)
        self.assertIn("RECOVERED_CONFIRMED slot=1 last_good=1", r_rec.stdout)
        self.assertIn("state=CONFIRMED consumed=1", self.inspect_txn(txid))

        # Subsequent confirm / replay after consumed=1 must strictly reject
        r_again = self.run_confirm()
        self.assertNotEqual(r_again.returncode, 0)
        self.assertIn("no active unconsumed transaction record", r_again.stderr)

    def test_n10_unprivileged_production_sgw_bootctl_and_override_rejected(self):
        if os.geteuid() == 0:
            self.skipTest("Must run as unprivileged user")
        prod_bootctl = BOOT / "sgw_bootctl"
        self.stage_signed_upgrade_to_slot1()
        before_env = self.get_env()

        r1 = subprocess.run([str(prod_bootctl), "confirm", "--running", "1"],
                            env=self.harness_env(), capture_output=True, text=True, timeout=10)
        self.assertNotEqual(r1.returncode, 0)
        self.assertIn("CONFIRM REJECTED", r1.stderr)

        r2 = subprocess.run([
            str(prod_bootctl), "-c", str(self.fw_cfg), "--cmdline", str(self.cmdline),
            "--mounts", str(self.mounts), "--sysroot", str(self.sysroot),
            "confirm", "--running", "1",
        ], env=self.harness_env(), capture_output=True, text=True, timeout=10)
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("CONFIRM REJECTED", r2.stderr)
        self.assertEqual(self.get_env(), before_env)


if __name__ == "__main__":
    unittest.main()
