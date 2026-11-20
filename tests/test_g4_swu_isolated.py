#!/usr/bin/env python3
"""G4 isolated virtual-disk production SWUpdate (.swu) end-to-end acceptance tests.

Exercises the real SWUpdate 2019.11 binary (CONFIG_SIGNED_IMAGES + CONFIG_SIGALG_RAWRSA
+ CONFIG_BOOTLOADER_NONE) invoking production bsp/ota/sgw-swu-apply.sh ->
sgw_bootctl_isolated (exact sgw_bootctl.c r3c_coordinator_apply_bundle code path)
against isolated virtual slot1/slot2/data/emmc_env images (never touching /dev/mmcblk1).
"""
import fcntl
import hashlib
import os
import re
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "bsp/ota/g4_swu_tool.py"
SWUPDATE_BIN = ROOT / "build/swupdate-host/swupdate"
BOOTCTL_ISOLATED = ROOT / "bsp/apps/boot_ab/sgw_bootctl_isolated"
BOOTCTL_PROD = ROOT / "bsp/apps/boot_ab/sgw_bootctl"
SWUPDATE_CFG = ROOT / "bsp/ota/swupdate.cfg"
UBOOT_BUILD = Path(os.environ.get("UBOOT_AB_BUILD_DIR", str(ROOT / "build/uboot-ab")))
FW_PRINTENV = Path(os.path.abspath(UBOOT_BUILD / "host/fw_printenv"))
FW_SETENV = Path(os.path.abspath(UBOOT_BUILD / "host/fw_setenv"))
ENV_OFFSET_0 = 0xE0000
ENV_OFFSET_1 = 0xE2000
ENV_SIZE = 0x2000

MANDATORY_FILES = {
    "/sbin/init": b"init-g4\n",
    "/etc/inittab": b"id:3:initdefault:\nsgw0::respawn:/bin/sh -c 'SMARTGATEWAY_RESPAWN=1 exec /opt/smartgateway/bin/service_supervisor_run'\n",
    "/etc/fstab": b"/dev/mmcblk1p3 /opt/smartgateway/data ext4 defaults 0 2\n",
    "/etc/fw_env.config": b"/dev/mmcblk1 0xe0000 0x2000\n/dev/mmcblk1 0xe2000 0x2000\n",
    "/sbin/fw_printenv": b"fw_printenv-bin\n",
    "/usr/bin/sgw_bootctl": b"sgw_bootctl-bin\n",
    "/usr/bin/sgw_watchdog_guard": b"sgw_watchdog_guard-bin\n",
    "/usr/bin/service_supervisor": b"service_supervisor-bin\n",
    "/usr/bin/smart_gateway": b"smart_gateway-bin\n",
    "/usr/bin/ota_agent": b"ota_agent-bin\n",
    "/etc/init.d/S15sgw_ab_guard": b"#!/bin/sh\n",
    "/etc/init.d/S99smartgateway": b"#!/bin/sh\n",
    "/boot/zImage": b"zImage-g4-kernel-payload\n",
    "/boot/100ask_imx6ull-14x14.dtb": b"dtb-g4-imx6ull\n",
    "/lib/libc.so.6": b"libc-2.30\n",
}


def find_fuse2fs():
    env = os.environ.get("SGW_TEST_FUSE2FS")
    if env and Path(env).exists():
        return env
    return shutil.which("fuse2fs")


FUSE2FS = find_fuse2fs()


def sha256_path(p: Path) -> str:
    h = hashlib.sha256()
    with p.open("rb") as f:
        while True:
            chunk = f.read(1024 * 1024)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


@unittest.skipUnless(FUSE2FS, "fuse2fs binary required for isolated ext4 DATA mount")
class TestG4SwuIsolated(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (TOOL, SWUPDATE_BIN, BOOTCTL_ISOLATED, BOOTCTL_PROD, SWUPDATE_CFG, FW_PRINTENV, FW_SETENV):
            if not p.exists():
                raise RuntimeError(f"missing required artifact: {p}")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".sgw-g4-", dir=Path.home())
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(self._unmount)
        self.base = Path(self.tmp.name)
        self.base.chmod(0o700)

        self.emmc_img = self.base / "emmc_env.img"
        with self.emmc_img.open("wb") as f:
            f.truncate(4 * 1024 * 1024)
        self.fw_cfg = self.base / "fw_env.config"
        self.fw_cfg.write_text(
            f"{self.emmc_img} 0x{ENV_OFFSET_0:x} 0x{ENV_SIZE:x}\n"
            f"{self.emmc_img} 0x{ENV_OFFSET_1:x} 0x{ENV_SIZE:x}\n",
            encoding="utf-8",
        )
        self._set_env({
            "sgw_active_slot": "1",
            "sgw_last_good_slot": "1",
            "sgw_pending_slot": "none",
            "sgw_ab_state": "confirmed",
            "bootlimit": "3",
            "bootcount": "0",
            "upgrade_available": "0",
            "sgw_rollback_count": "0",
            "sgw_last_error": "none",
        })

        self.slot1_img = self.base / "slot1.ext4"
        self.slot2_img = self.base / "slot2.ext4"
        with self.slot1_img.open("wb") as f:
            f.write(b"SLOT1_GOLDEN_" * (8 * 1024 * 1024 // 13))
            f.truncate(8 * 1024 * 1024)
        with self.slot2_img.open("wb") as f:
            f.truncate(16 * 1024 * 1024)

        self.data_img = self.base / "data.ext4"
        self.data_mnt = self.base / "data"
        self.data_mnt.mkdir(mode=0o700)
        self.mounted = False
        with self.data_img.open("wb") as f:
            f.truncate(32 * 1024 * 1024)
        subprocess.run(["mke2fs", "-q", "-F", "-t", "ext4", str(self.data_img)], check=True)
        uid, gid = os.getuid(), os.getgid()
        for key, val in (("uid", uid), ("gid", gid)):
            subprocess.run(["debugfs", "-w", "-R", f"set_inode_field <2> {key} {val}", str(self.data_img)],
                           check=True, capture_output=True)
        self._mount_data()
        schema = self.data_mnt / ".sgw-data-schema"
        schema.write_text("schema=1\n", encoding="utf-8")
        schema.chmod(0o600)
        self.txdir = self.data_mnt / ".sgw-tx"
        self.txdir.mkdir(mode=0o700)

        self.app_ota_dir = self.base / "app"
        self.app_ota_dir.mkdir(mode=0o700)
        (self.app_ota_dir / "state").mkdir(mode=0o700)
        (self.app_ota_dir / "releases/0.6.0").mkdir(parents=True, mode=0o700)
        self.ota_lock = self.app_ota_dir / ".ota.lock"
        self.ota_lock.write_bytes(b"")
        self.ota_lock.chmod(0o600)
        ota_state = self.app_ota_dir / "state/ota_state.json"
        ota_state.write_text(
            '{"schema_version":1,"state":"CONFIRMED","confirmed_version":"0.6.0","candidate_version":"0.6.0"}\n',
            encoding="utf-8",
        )
        ota_state.chmod(0o600)
        (self.app_ota_dir / "current").symlink_to("releases/0.6.0")

        self.sysroot = self.base / "sysroot"
        self.sysroot.mkdir(mode=0o755)
        self.cmdline = self.base / "cmdline"
        self.mounts = self.base / "mounts"
        self.mountinfo = self.base / "mountinfo"
        self._set_running_slot(1)

        self.privkey = self.base / "ota-private.pem"
        self.pubkey = self.base / "ota-public.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                        "-out", str(self.privkey)], check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(self.privkey), "-pubout",
                        "-out", str(self.pubkey)], check=True, capture_output=True)
        self.pubkey.chmod(0o600)

        self.payload_tree = self.base / "payload_tree"
        rm_lines = []
        for rel, content in MANDATORY_FILES.items():
            fp = self.payload_tree / rel.lstrip("/")
            fp.parent.mkdir(parents=True, exist_ok=True)
            fp.write_bytes(content)
            rm_lines.append(f"{rel} {hashlib.sha256(content).hexdigest()}")
        self.rootfs_manifest = self.base / "rootfs.manifest"
        self.rootfs_manifest.write_text("\n".join(rm_lines) + "\n", encoding="utf-8")
        self.zimage = self.payload_tree / "boot/zImage"
        self.dtb = self.payload_tree / "boot/100ask_imx6ull-14x14.dtb"

        self.payload_ext4 = self.base / "payload.ext4"
        with self.payload_ext4.open("wb") as f:
            f.truncate(8 * 1024 * 1024)
        subprocess.run(["mke2fs", "-q", "-F", "-d", str(self.payload_tree), "-t", "ext4", str(self.payload_ext4)],
                       check=True, capture_output=True)

    def _mount_data(self):
        subprocess.run([FUSE2FS, str(self.data_img), str(self.data_mnt)],
                       check=True, capture_output=True, timeout=10)
        self.mounted = True
        self.data_mnt.chmod(0o700)

    def _unmount(self):
        if getattr(self, "mounted", False):
            subprocess.run(["fusermount", "-u", str(self.data_mnt)], capture_output=True)
            self.mounted = False

    def _set_env(self, kv: dict):
        script = self.base / "fw_script.txt"
        script.write_text("".join(f"{k} {v}\n" for k, v in kv.items()), encoding="utf-8")
        subprocess.run([str(FW_SETENV), "-c", str(self.fw_cfg), "-s", str(script)], check=True, capture_output=True)

    def _get_env(self) -> dict:
        r = subprocess.run([str(FW_PRINTENV), "-c", str(self.fw_cfg)], check=True, capture_output=True, text=True)
        return dict(line.split("=", 1) for line in r.stdout.splitlines() if "=" in line)

    def _set_running_slot(self, slot: int):
        self.cmdline.write_text(f"console=ttymxc0,115200 root=/dev/mmcblk1p{slot} rootwait rw\n", encoding="utf-8")
        self.mounts.write_text(
            f"/dev/mmcblk1p{slot} / ext4 rw,relatime 0 0\n/dev/mmcblk1p3 /opt/smartgateway/data ext4 rw,relatime 0 0\n",
            encoding="utf-8",
        )
        self.mountinfo.write_text(
            f"10 1 179:{slot} / / rw,relatime - ext4 /dev/mmcblk1p{slot} rw\n"
            "11 10 179:3 / /opt/smartgateway/data rw,relatime - ext4 /dev/mmcblk1p3 rw\n",
            encoding="utf-8",
        )

    def _pack_swu(self, out_name: str, target_slot: int, board: str = "imx6ull-100ask",
                  hw_rev: str = "revA", privkey: Path = None) -> Path:
        swu = self.base / out_name
        subprocess.run(
            [
                "python3", str(TOOL), "pack",
                "--rootfs-img", str(self.payload_ext4),
                "--zimage", str(self.zimage),
                "--dtb", str(self.dtb),
                "--rootfs-manifest", str(self.rootfs_manifest),
                "--privkey", str(privkey or self.privkey),
                "--target-slot", str(target_slot),
                "--board", board,
                "--hw-rev", hw_rev,
                "--out", str(swu),
            ],
            check=True, capture_output=True, text=True,
        )
        return swu

    def _pack_cpio_from_dir(self, src_dir: Path, entries: list, out_swu: Path):
        with out_swu.open("wb") as out_f:
            subprocess.run(
                ["cpio", "-o", "-H", "crc", "--quiet"],
                cwd=str(src_dir),
                input=("\n".join(entries) + "\n").encode("utf-8"),
                stdout=out_f,
                check=True,
            )

    def _apply_swu(self, swu: Path, slot_img: Path, expected_target=None,
                   hw_rev: str = "revA", swupdate_cfg: Path = None, extra_env: dict = None,
                   mountinfo="/proc/self/mountinfo", allow_virtual_slot2: bool = False,
                   fuse_data: bool = True, policy: Path = None, data_mnt: Path = None,
                   txdir: Path = None):
        cmd = [
            "python3", str(TOOL), "apply",
            "--swu", str(swu),
            "--pubkey", str(self.pubkey),
            "--slot-img", str(slot_img),
            "--txdir", str(txdir or self.txdir),
            "--fw-cfg", str(self.fw_cfg),
            "--fw-printenv", str(FW_PRINTENV),
            "--fw-setenv", str(FW_SETENV),
            "--cmdline", str(self.cmdline),
            "--mounts", str(self.mounts),
            "--mountinfo", str(mountinfo),
            "--sysroot", str(self.sysroot),
            "--app-ota-dir", str(self.app_ota_dir),
            "--data-mnt", str(data_mnt or self.data_mnt),
            "--hw-rev", hw_rev,
        ]
        if expected_target is not None:
            cmd.extend(["--expected-target", str(expected_target)])
        if swupdate_cfg is not None:
            cmd.extend(["--swupdate-cfg", str(swupdate_cfg)])
        if policy is not None:
            cmd.extend(["--policy", str(policy)])
        if allow_virtual_slot2:
            cmd.append("--allow-virtual-slot2")
        if not fuse_data:
            cmd.append("--no-fuse-data")
        env = os.environ.copy()
        if extra_env:
            env.update(extra_env)
        return subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=240)

    def _parse_txn(self, txid: str) -> dict:
        txn_file = self.txdir / f"txn-{txid}"
        manifest_file = self.txdir / f"txn-{txid}.manifest"
        self.assertTrue(txn_file.is_file(), f"missing {txn_file}")
        self.assertTrue(manifest_file.is_file(), f"missing {manifest_file}")
        self.assertEqual(manifest_file.stat().st_mode & 0o777, 0o600)
        lines = txn_file.read_text(encoding="utf-8").splitlines()
        self.assertGreaterEqual(len(lines), 1)
        self.assertEqual(lines[0], "SGW-TXN-1")
        parsed = {}
        for line in lines[1:]:
            if "=" in line:
                k, v = line.split("=", 1)
                parsed[k] = v
        return parsed

    def test_p1_to_p2_real_swupdate_apply_and_pending_stage(self):
        slot1_before = sha256_path(self.slot1_img)
        payload_sha = sha256_path(self.payload_ext4)
        cfg_copy = self.base / "swupdate.cfg"
        cfg_copy.write_text(SWUPDATE_CFG.read_text(encoding="utf-8"), encoding="utf-8")

        swu = self._pack_swu("g4-to-p2.swu", target_slot=2)
        res = self._apply_swu(
            swu, self.slot2_img, expected_target=2,
            swupdate_cfg=cfg_copy, allow_virtual_slot2=True,
        )
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("STAGED tx=", res.stdout)
        self.assertIn("Software updated successfully", res.stdout)
        txid = res.stdout.split("tx=")[1].split()[0]

        written_prefix = self.slot2_img.read_bytes()[: self.payload_ext4.stat().st_size]
        self.assertEqual(hashlib.sha256(written_prefix).hexdigest(), payload_sha)
        self.assertEqual(sha256_path(self.slot1_img), slot1_before)

        env = self._get_env()
        self.assertEqual(env["sgw_pending_slot"], "2")
        self.assertEqual(env["sgw_last_good_slot"], "1")
        self.assertEqual(env["sgw_ab_state"], "trial")
        self.assertEqual(env["upgrade_available"], "1")
        self.assertEqual(env["bootcount"], "0")

        txn = self._parse_txn(txid)
        self.assertEqual(txn["state"], "PENDING")
        self.assertEqual(txn["target"], "2")
        self.assertEqual(txn["last_good"], "1")
        self.assertEqual(txn.get("consumed", "0"), "0")

    def test_p2_to_p1_real_swupdate_apply_and_pending_stage(self):
        self._set_running_slot(2)
        self._set_env({
            "sgw_active_slot": "2",
            "sgw_last_good_slot": "2",
            "sgw_pending_slot": "none",
            "sgw_ab_state": "confirmed",
            "bootlimit": "3",
            "bootcount": "0",
            "upgrade_available": "0",
        })
        slot2_before = sha256_path(self.slot2_img)
        payload_sha = sha256_path(self.payload_ext4)
        swu = self._pack_swu("g4-to-p1.swu", target_slot=1)
        # Default policy (protect_slot2=1) MUST allow p2 -> p1 writes!
        res = self._apply_swu(swu, self.slot1_img, expected_target=1)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(sha256_path(self.slot1_img), payload_sha)
        self.assertEqual(sha256_path(self.slot2_img), slot2_before)
        env = self._get_env()
        self.assertEqual(env["sgw_pending_slot"], "1")
        self.assertEqual(env["sgw_last_good_slot"], "2")
        self.assertEqual(env["sgw_ab_state"], "trial")
        self.assertEqual(env["upgrade_available"], "1")

    def test_real_g3_artifacts_swu_p1_to_p2_via_real_swupdate(self):
        real_img = ROOT / "build/buildroot-g3/images/rootfs.ext4"
        real_zimg = ROOT / "build/kernel-g3/trimmed/out/zImage"
        real_dtb = ROOT / "build/kernel-g3/trimmed/out/100ask_imx6ull-14x14.dtb"
        real_tar = ROOT / "build/buildroot-g3/images/rootfs.tar"
        if not (real_img.exists() and real_zimg.exists() and real_dtb.exists() and real_tar.exists()):
            self.skipTest("clean G3 images not present")
        import tarfile
        rm_lines = []
        with tarfile.open(real_tar) as tf:
            members = {m.name.lstrip("./"): m for m in tf.getmembers()}
            for rel in MANDATORY_FILES:
                f = tf.extractfile(members[rel.lstrip("/")])
                self.assertIsNotNone(f, rel)
                rm_lines.append(f"{rel} {hashlib.sha256(f.read()).hexdigest()}")
        real_rm = self.base / "real_g3_rootfs.manifest"
        real_rm.write_text("\n".join(rm_lines) + "\n", encoding="utf-8")

        real_swu = self.base / "real-g3-to-p2.swu"
        subprocess.run(
            [
                "python3", str(TOOL), "pack",
                "--rootfs-img", str(real_img),
                "--zimage", str(real_zimg),
                "--dtb", str(real_dtb),
                "--rootfs-manifest", str(real_rm),
                "--privkey", str(self.privkey),
                "--target-slot", "2",
                "--version", "g3-clean-g4",
                "--out", str(real_swu),
            ],
            check=True, capture_output=True, text=True,
        )
        virt_p2 = self.base / "virt_real_p2.ext4"
        with virt_p2.open("wb") as f:
            f.truncate(160 * 1024 * 1024)
        s1_before = sha256_path(self.slot1_img)
        res = self._apply_swu(real_swu, virt_p2, expected_target=2, allow_virtual_slot2=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        real_sz = real_img.stat().st_size
        with virt_p2.open("rb") as f:
            written = f.read(real_sz)
        self.assertEqual(hashlib.sha256(written).hexdigest(), sha256_path(real_img))
        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        env = self._get_env()
        self.assertEqual(env["sgw_pending_slot"], "2")
        self.assertEqual(env["sgw_ab_state"], "trial")
        self.assertEqual(env["upgrade_available"], "1")

    def test_protected_factory_slot2_rejected_zero_mutation(self):
        s1_before = sha256_path(self.slot1_img)
        s2_before = sha256_path(self.slot2_img)
        env_before = sha256_path(self.emmc_img)

        # (1) Running on p1 (confirmed): default policy protects factory slot 2 -> MUST reject!
        swu_to_p2 = self._pack_swu("protected-p2.swu", target_slot=2)
        r1 = self._apply_swu(swu_to_p2, self.slot2_img, expected_target=2)
        self.assertNotEqual(r1.returncode, 0)
        self.assertIn("write-protected factory baseline", r1.stderr)
        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

        # (1b) Omitting protect_slot2 in policy MUST default fail-closed to protect_slot2=1
        pol_omit = self.base / "omit_protect.conf"
        pol_omit.write_text(
            f"data_mountpoint={self.data_mnt}\n"
            "data_device=/dev/mmcblk1p3\n"
            "data_min_bytes=1048576\n"
            f"app_ota_dir={self.app_ota_dir}\n"
            f"tx_dir={self.txdir}\n"
            f"pubkey_path={self.pubkey}\n"
            "slot1_device=/dev/mmcblk1p1\n"
            "slot2_device=/dev/mmcblk1p2\n"
            "whole_disk_device=/dev/mmcblk1\n"
            f"supervisor_sock={self.app_ota_dir / 'run/supervisor.sock'}\n"
            f"supervisor_exe={self.app_ota_dir / 'bin/service_supervisor'}\n"
            f"gateway_sock={self.app_ota_dir / 'run/gateway-health.sock'}\n"
            f"gateway_exe={self.app_ota_dir / 'current/smart_gateway'}\n"
            "net_iface=eth0\n"
            "gateway_flow_mode=controlled_local\n"
            "allow_external_degraded=1\n"
            "require_sensor_heartbeat=0\n"
            "stable_ms=150\n"
            "max_gap_ms=400\n"
            "deadline_ms=2000\n"
            "max_evidence_age_ms=2000\n",
            encoding="utf-8",
        )
        pol_omit.chmod(0o600)
        r1b = self._apply_swu(swu_to_p2, self.slot2_img, expected_target=2, policy=pol_omit)
        self.assertNotEqual(r1b.returncode, 0)
        self.assertIn("write-protected factory baseline", r1b.stderr)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

        # (2) Production binary sgw_bootctl apply-bundle --target 2 MUST reject even under LD_PRELOAD uid 0
        bdir = self.base / "dummy_p2_bundle"
        bdir.mkdir(mode=0o700)
        uid_c = self.base / "uid0_p2.c"
        uid_so = self.base / "uid0_p2.so"
        uid_c.write_text("#include <sys/types.h>\nuid_t geteuid(void) { return 0; }\n", encoding="utf-8")
        subprocess.run(["gcc", "-shared", "-fPIC", str(uid_c), "-o", str(uid_so)], check=True)
        r2 = subprocess.run(
            [str(BOOTCTL_PROD), "apply-bundle", "--bundle", str(bdir), "--target", "2"],
            env=dict(os.environ, LD_PRELOAD=str(uid_so)),
            capture_output=True, text=True, timeout=10,
        )
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("APPLY REJECTED", r2.stderr)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)

    def test_shared_data_p3_unmounted_wrong_mount_bad_schema_and_outside_txdir_rejected(self):
        self._set_running_slot(2)
        self._set_env({
            "sgw_active_slot": "2",
            "sgw_last_good_slot": "2",
            "sgw_pending_slot": "none",
            "sgw_ab_state": "confirmed",
            "bootlimit": "3",
            "bootcount": "0",
            "upgrade_available": "0",
        })
        s1_before = sha256_path(self.slot1_img)
        s2_before = sha256_path(self.slot2_img)
        env_before = sha256_path(self.emmc_img)
        good_p1_swu = self._pack_swu("data-gate-p1.swu", target_slot=1)

        # (a) p3 unmounted (even with local fake .sgw-data-schema & .sgw-tx on unmounted mount dir)
        self._unmount()
        try:
            self.data_mnt.chmod(0o700)
            fake_schema = self.data_mnt / ".sgw-data-schema"
            fake_schema.write_text("schema=1\n", encoding="utf-8")
            fake_schema.chmod(0o600)
            fake_tx = self.data_mnt / ".sgw-tx"
            fake_tx.mkdir(mode=0o700)
            r_unmnt = self._apply_swu(good_p1_swu, self.slot1_img, expected_target=1)
            self.assertNotEqual(r_unmnt.returncode, 0)
            self.assertTrue(
                "not an isolated mount" in r_unmnt.stderr or "mountinfo" in r_unmnt.stderr,
                r_unmnt.stderr,
            )
            self.assertEqual(list(fake_tx.glob("txn-*")), [])
        finally:
            shutil.rmtree(self.data_mnt / ".sgw-tx", ignore_errors=True)
            (self.data_mnt / ".sgw-data-schema").unlink(missing_ok=True)
            self._mount_data()

        # (b) p3 wrongly mounted via synthetic mountinfo cases
        dev_num = self.data_mnt.stat().st_dev
        d_maj, d_min = os.major(dev_num), os.minor(dev_num)
        for tag, mi_line in (
            ("ro", f"11 10 {d_maj}:{d_min} / {self.data_mnt} ro - fuse.ext4 {self.data_img} rw\n"),
            ("fstype", f"11 10 {d_maj}:{d_min} / {self.data_mnt} rw - tmpfs tmpfs rw\n"),
            ("devno", f"11 10 0:999 / {self.data_mnt} rw - fuse.ext4 {self.data_img} rw\n"),
            ("bindroot", f"11 10 {d_maj}:{d_min} /subdir {self.data_mnt} rw - fuse.ext4 {self.data_img} rw\n"),
        ):
            mi_file = self.base / f"mi_{tag}"
            mi_file.write_text(mi_line, encoding="utf-8")
            r_mi = self._apply_swu(good_p1_swu, self.slot1_img, expected_target=1, mountinfo=mi_file)
            self.assertNotEqual(r_mi.returncode, 0, tag)
            self.assertIn("mountinfo", r_mi.stderr)

        # (c) p3 bad or missing .sgw-data-schema
        schema = self.data_mnt / ".sgw-data-schema"
        try:
            schema.write_text("schema=2\n", encoding="utf-8")
            schema.chmod(0o600)
            r_bad_sch = self._apply_swu(good_p1_swu, self.slot1_img, expected_target=1)
            self.assertNotEqual(r_bad_sch.returncode, 0)
            self.assertIn("DATA schema", r_bad_sch.stderr)

            schema.unlink()
            r_no_sch = self._apply_swu(good_p1_swu, self.slot1_img, expected_target=1)
            self.assertNotEqual(r_no_sch.returncode, 0)
            self.assertIn("DATA schema", r_no_sch.stderr)
        finally:
            schema.write_text("schema=1\n", encoding="utf-8")
            schema.chmod(0o600)

        # (d) tx_dir outside data_mountpoint while p3 is mounted
        rogue_tx = self.base / "rogue_tx"
        rogue_tx.mkdir(mode=0o700)
        r_out = self._apply_swu(good_p1_swu, self.slot1_img, expected_target=1, txdir=rogue_tx)
        self.assertNotEqual(r_out.returncode, 0)
        self.assertIn("outside DATA mountpoint", r_out.stderr)
        self.assertEqual(list(rogue_tx.glob("txn-*")), [])

        # (e) Production block-DATA path (--no-fuse-data -> r3c_data_inspect_full)
        r_prod_data = self._apply_swu(good_p1_swu, self.slot1_img, expected_target=1, fuse_data=False)
        self.assertNotEqual(r_prod_data.returncode, 0)
        self.assertIn("APPLY REJECTED", r_prod_data.stderr)

        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

    def test_negative_swupdate_rejections_zero_mutation(self):
        s1_before = sha256_path(self.slot1_img)
        s2_before = sha256_path(self.slot2_img)
        env_before = sha256_path(self.emmc_img)

        good_swu = self._pack_swu("base-good.swu", target_slot=2)
        base_entries = [
            "sw-description",
            "sw-description.sig",
            "sgw-swu-apply.sh",
            "manifest",
            "manifest.sig",
            "rootfs.manifest",
            "zImage",
            "100ask_imx6ull-14x14.dtb",
            "rootfs.img",
        ]

        udir = self.base / "neg_unsigned"
        udir.mkdir()
        subprocess.run(["cpio", "-iu", "--quiet"], cwd=str(udir), input=good_swu.read_bytes(), check=True)
        unsigned_swu = self.base / "unsigned.swu"
        self._pack_cpio_from_dir(udir, [e for e in base_entries if e != "sw-description.sig"], unsigned_swu)
        r = self._apply_swu(unsigned_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)

        bad_priv = self.base / "untrusted.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                        "-out", str(bad_priv)], check=True, capture_output=True)
        bad_key_swu = self._pack_swu("bad-key.swu", target_slot=2, privkey=bad_priv)
        r = self._apply_swu(bad_key_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)

        tsig_dir = self.base / "neg_sig"
        tsig_dir.mkdir()
        subprocess.run(["cpio", "-iu", "--quiet"], cwd=str(tsig_dir), input=good_swu.read_bytes(), check=True)
        sig_bytes = bytearray((tsig_dir / "sw-description.sig").read_bytes())
        sig_bytes[0] ^= 0xFF
        (tsig_dir / "sw-description.sig").write_bytes(bytes(sig_bytes))
        tamper_sig_swu = self.base / "tamper-sig.swu"
        self._pack_cpio_from_dir(tsig_dir, base_entries, tamper_sig_swu)
        r = self._apply_swu(tamper_sig_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)

        for fname in ("rootfs.img", "zImage", "100ask_imx6ull-14x14.dtb", "sgw-swu-apply.sh"):
            tdir = self.base / f"neg_tamper_{fname.replace('.', '_')}"
            tdir.mkdir()
            subprocess.run(["cpio", "-iu", "--quiet"], cwd=str(tdir), input=good_swu.read_bytes(), check=True)
            data = bytearray((tdir / fname).read_bytes())
            off = min(4096, len(data) - 1) if len(data) > 0 else 0
            data[off] ^= 0xFF
            (tdir / fname).write_bytes(bytes(data))
            out = self.base / f"tamper-{fname}.swu"
            self._pack_cpio_from_dir(tdir, base_entries, out)
            r = self._apply_swu(out, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
            self.assertNotEqual(r.returncode, 0, fname)
            self.assertIn("HASH mismatch", r.stderr)

        wrong_board_swu = self._pack_swu("wrong-board.swu", target_slot=2, board="other-board")
        r = self._apply_swu(wrong_board_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)

        r = self._apply_swu(good_swu, self.slot2_img, expected_target=2, hw_rev="revB", allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("not compatible with hardware", r.stderr)

        bdir = self.base / "neg_bootenv"
        bdir.mkdir()
        (bdir / "sw-description").write_text(
            "software =\n"
            "{\n"
            "\timx6ull-100ask = {\n"
            '\t\tversion = "g4-bootenv";\n'
            '\t\thardware-compatibility = [ "revA" ];\n'
            "\t\tbootenv: (\n"
            '\t\t\t{ name = "sgw_active_slot"; value = "2"; }\n'
            "\t\t);\n"
            "\t};\n"
            "}\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(self.privkey),
             "-out", str(bdir / "sw-description.sig"), str(bdir / "sw-description")],
            check=True, capture_output=True,
        )
        bootenv_swu = self.base / "bootenv-bypass.swu"
        self._pack_cpio_from_dir(bdir, ["sw-description", "sw-description.sig"], bootenv_swu)
        r = self._apply_swu(bootenv_swu, self.slot2_img, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("bootloader support absent", r.stderr)

        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

    def test_negative_bootctl_rejections_zero_mutation(self):
        s1_before = sha256_path(self.slot1_img)
        s2_before = sha256_path(self.slot2_img)
        env_before = sha256_path(self.emmc_img)
        good_swu = self._pack_swu("base-good-ctl.swu", target_slot=2)
        base_entries = [
            "sw-description",
            "sw-description.sig",
            "sgw-swu-apply.sh",
            "manifest",
            "manifest.sig",
            "rootfs.manifest",
            "zImage",
            "100ask_imx6ull-14x14.dtb",
            "rootfs.img",
        ]

        # (a) Wrong target slot (targeting currently running & last_good slot 1)
        wrong_slot_swu = self._pack_swu("wrong-slot.swu", target_slot=1)
        r = self._apply_swu(wrong_slot_swu, self.slot1_img)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("APPLY REJECTED", r.stderr)

        # (b) Slot capacity smaller than rootfs.img -> rejected with ZERO bytes written
        small_slot = self.base / "small_slot.ext4"
        with small_slot.open("wb") as f:
            f.write(b"SMALL_SLOT_KEEP" * 1024)
            f.truncate(2 * 1024 * 1024)
        small_sha = sha256_path(small_slot)
        r = self._apply_swu(good_swu, small_slot, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("capacity", r.stderr)
        self.assertEqual(sha256_path(small_slot), small_sha)

        # (c) Concurrent .ota.lock contention -> rejected before writing
        with self.ota_lock.open("r+b") as lockf:
            fcntl.flock(lockf.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            try:
                r = self._apply_swu(good_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
            finally:
                fcntl.flock(lockf.fileno(), fcntl.LOCK_UN)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("is busy", r.stderr)

        # (d) Active trial state in U-Boot env -> rejected before writing
        self._set_env({
            "sgw_active_slot": "1",
            "sgw_last_good_slot": "1",
            "sgw_pending_slot": "2",
            "sgw_ab_state": "trial",
            "bootlimit": "3",
            "bootcount": "0",
            "upgrade_available": "1",
        })
        r = self._apply_swu(good_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("trial", r.stderr)
        self._set_env({
            "sgw_active_slot": "1",
            "sgw_last_good_slot": "1",
            "sgw_pending_slot": "none",
            "sgw_ab_state": "confirmed",
            "bootlimit": "3",
            "bootcount": "0",
            "upgrade_available": "0",
        })
        env_before = sha256_path(self.emmc_img)

        # (e) Validly signed outer .swu with tampered inner manifest.sig
        mdir = self.base / "neg_inner_msig"
        mdir.mkdir()
        subprocess.run(["cpio", "-iu", "--quiet"], cwd=str(mdir), input=good_swu.read_bytes(), check=True)
        old_msig_sha = sha256_path(mdir / "manifest.sig")
        msig_bytes = bytearray((mdir / "manifest.sig").read_bytes())
        msig_bytes[0] ^= 0xFF
        (mdir / "manifest.sig").write_bytes(bytes(msig_bytes))
        new_msig_sha = sha256_path(mdir / "manifest.sig")
        desc = (mdir / "sw-description").read_text(encoding="utf-8")
        self.assertIn(old_msig_sha, desc)
        (mdir / "sw-description").write_text(desc.replace(old_msig_sha, new_msig_sha), encoding="utf-8")
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(self.privkey),
             "-out", str(mdir / "sw-description.sig"), str(mdir / "sw-description")],
            check=True, capture_output=True,
        )
        inner_msig_swu = self.base / "inner-tamper-msig.swu"
        self._pack_cpio_from_dir(mdir, base_entries, inner_msig_swu)
        r = self._apply_swu(inner_msig_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("signed bundle verification failed", r.stderr)

        # (f) Validly signed outer .swu whose top-level zImage differs from /boot/zImage inside rootfs.img
        zdir = self.base / "neg_zimage_diverge"
        zdir.mkdir()
        subprocess.run(["cpio", "-iu", "--quiet"], cwd=str(zdir), input=good_swu.read_bytes(), check=True)
        old_z_sha = sha256_path(zdir / "zImage")
        (zdir / "zImage").write_bytes(b"different-unmatched-zImage\n")
        new_z_sha = sha256_path(zdir / "zImage")
        zdesc = (zdir / "sw-description").read_text(encoding="utf-8")
        self.assertIn(old_z_sha, zdesc)
        (zdir / "sw-description").write_text(zdesc.replace(old_z_sha, new_z_sha), encoding="utf-8")
        subprocess.run(
            ["openssl", "dgst", "-sha256", "-sign", str(self.privkey),
             "-out", str(zdir / "sw-description.sig"), str(zdir / "sw-description")],
            check=True, capture_output=True,
        )
        z_div_swu = self.base / "zimage-diverge.swu"
        self._pack_cpio_from_dir(zdir, base_entries, z_div_swu)
        r = self._apply_swu(z_div_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("kernel/dtb digest diverges", r.stderr)

        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

    def test_fsync_failure_during_slot_write_never_stages_pending(self):
        s1_before = sha256_path(self.slot1_img)
        env_before = sha256_path(self.emmc_img)
        payload_sha = sha256_path(self.payload_ext4)

        fsync_c = self.base / "fsync_slot_fail.c"
        fsync_so = self.base / "libfsync_slot_fail.so"
        fsync_c.write_text(
            '#define _GNU_SOURCE\n'
            '#include <dlfcn.h>\n'
            '#include <errno.h>\n'
            '#include <stdio.h>\n'
            '#include <string.h>\n'
            '#include <unistd.h>\n'
            'int fsync(int fd) {\n'
            '    int (*real_fsync)(int) = dlsym(RTLD_NEXT, "fsync");\n'
            '    char lnk[64], target[1024];\n'
            '    ssize_t n;\n'
            '    snprintf(lnk, sizeof(lnk), "/proc/self/fd/%d", fd);\n'
            '    n = readlink(lnk, target, sizeof(target) - 1);\n'
            '    if (n > 0) {\n'
            '        target[n] = 0;\n'
            '        const char *suffix = "/slot2.ext4";\n'
            '        size_t slen = strlen(suffix);\n'
            '        if ((size_t)n >= slen && strcmp(target + n - slen, suffix) == 0) {\n'
            '            errno = EIO;\n'
            '            return -1;\n'
            '        }\n'
            '    }\n'
            '    return real_fsync(fd);\n'
            '}\n',
            encoding="utf-8",
        )
        subprocess.run(["gcc", "-shared", "-fPIC", str(fsync_c), "-ldl", "-o", str(fsync_so)], check=True)

        good_swu = self._pack_swu("fsync-fail.swu", target_slot=2)
        r_fail = self._apply_swu(
            good_swu,
            self.slot2_img,
            expected_target=2,
            allow_virtual_slot2=True,
            extra_env={"LD_PRELOAD": str(fsync_so)},
        )
        self.assertNotEqual(r_fail.returncode, 0)
        self.assertIn("slot fsync failed", r_fail.stderr)
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        env = self._get_env()
        self.assertEqual(env["sgw_ab_state"], "confirmed")
        self.assertEqual(env["upgrade_available"], "0")
        self.assertEqual(env["sgw_pending_slot"], "none")
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

        r_ok = self._apply_swu(good_swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertEqual(r_ok.returncode, 0, r_ok.stdout + r_ok.stderr)
        self.assertIn("STAGED tx=", r_ok.stdout)
        written_prefix = self.slot2_img.read_bytes()[: self.payload_ext4.stat().st_size]
        self.assertEqual(hashlib.sha256(written_prefix).hexdigest(), payload_sha)
        env = self._get_env()
        self.assertEqual(env["sgw_ab_state"], "trial")
        self.assertEqual(env["upgrade_available"], "1")
        self.assertEqual(env["sgw_pending_slot"], "2")

    def test_production_sgw_bootctl_apply_bundle_rejects_unprivileged_and_overrides(self):
        if os.geteuid() == 0:
            self.skipTest("Must run as unprivileged user")
        s1_before = sha256_path(self.slot1_img)
        s2_before = sha256_path(self.slot2_img)
        env_before = sha256_path(self.emmc_img)

        bdir = self.base / "dummy_bundle"
        bdir.mkdir(mode=0o700)
        pol = self.base / "dummy_pol.conf"
        pol.write_text(f"tx_dir={self.txdir}\n", encoding="utf-8")
        pol.chmod(0o600)

        for cmd in (
            [str(BOOTCTL_PROD), "apply-bundle", "--bundle", str(bdir), "--target", "1"],
            [str(BOOTCTL_PROD), "apply-bundle", "--bundle", str(bdir), "--target", "2"],
            [str(BOOTCTL_PROD), "-c", str(self.fw_cfg), "apply-bundle", "--bundle", str(bdir), "--target", "1"],
            [str(BOOTCTL_PROD), "apply-bundle", "--bundle", str(bdir), "--target", "1",
             "--policy", str(pol), "--slot-img", str(self.slot1_img)],
            [str(BOOTCTL_PROD), "apply-bundle", "--bundle", str(bdir), "--target", "1", "--fuse-data"],
        ):
            r = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
            self.assertNotEqual(r.returncode, 0)
            self.assertIn("APPLY REJECTED", r.stderr)

        uid_c = self.base / "uid0.c"
        uid_so = self.base / "uid0.so"
        uid_c.write_text("#include <sys/types.h>\nuid_t geteuid(void) { return 0; }\n", encoding="utf-8")
        subprocess.run(["gcc", "-shared", "-fPIC", str(uid_c), "-o", str(uid_so)], check=True)
        for override_args in (
            ["--policy", str(pol), "--slot-img", str(self.slot1_img)],
            ["--fuse-data"],
        ):
            r_fake_root = subprocess.run(
                [str(BOOTCTL_PROD), "apply-bundle", "--bundle", str(bdir), "--target", "1", *override_args],
                env=dict(os.environ, LD_PRELOAD=str(uid_so)),
                capture_output=True, text=True, timeout=10,
            )
            self.assertNotEqual(r_fake_root.returncode, 0)
            self.assertIn("APPLY REJECTED: production apply-bundle forbids path overrides", r_fake_root.stderr)

        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)

    def test_sequential_upgrade_after_prior_confirmed_txn_in_txdir(self):
        settled_id = "a" * 64
        settled_txn = self.txdir / f"txn-{settled_id}"
        settled_mf = self.txdir / f"txn-{settled_id}.manifest"
        settled_text = (
            f"SGW-TXN-1\nstate=CONFIRMED\nid={settled_id}\ntarget=1\nlast_good=2\nconsumed=1\n"
        )
        settled_mf_text = "/sbin/init " + ("b" * 64) + "\n"
        settled_txn.write_text(settled_text, encoding="utf-8")
        settled_mf.write_text(settled_mf_text, encoding="utf-8")
        settled_txn.chmod(0o600)
        settled_mf.chmod(0o600)

        swu = self._pack_swu("seq-p2.swu", target_slot=2)
        res = self._apply_swu(swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        m = re.search(r"STAGED tx=([0-9a-f]{64}) target=2 last_good=1", res.stdout)
        self.assertIsNotNone(m, res.stdout + res.stderr)
        new_txid = m.group(1)
        self.assertNotEqual(new_txid, settled_id)
        parsed = self._parse_txn(new_txid)
        self.assertEqual(parsed["state"], "PENDING")
        self.assertEqual(parsed["target"], "2")
        self.assertEqual(settled_txn.read_text(encoding="utf-8"), settled_text)
        self.assertEqual(settled_mf.read_text(encoding="utf-8"), settled_mf_text)

    def test_readback_digest_mismatch_never_stages_pending(self):
        env_before = sha256_path(self.emmc_img)
        corrupt_c = self.base / "readback_corrupt.c"
        corrupt_so = self.base / "readback_corrupt.so"
        corrupt_c.write_text(
            "#define _GNU_SOURCE\n"
            "#include <dlfcn.h>\n"
            "#include <stdio.h>\n"
            "#include <string.h>\n"
            "#include <sys/types.h>\n"
            "#include <unistd.h>\n"
            "static void maybe_corrupt(int fd, void *buf, off_t offset, ssize_t r) {\n"
            "    char link[64], target[512];\n"
            "    if (r <= 0 || offset != 0) return;\n"
            "    snprintf(link, sizeof(link), \"/proc/self/fd/%d\", fd);\n"
            "    ssize_t n = readlink(link, target, sizeof(target) - 1);\n"
            "    if (n > 0) {\n"
            "        target[n] = '\\0';\n"
            "        const char *sfx = \"/slot2.ext4\";\n"
            "        size_t tlen = strlen(target), slen = strlen(sfx);\n"
            "        if (tlen >= slen && strcmp(target + tlen - slen, sfx) == 0) {\n"
            "            ((unsigned char *)buf)[0] ^= 0x01;\n"
            "        }\n"
            "    }\n"
            "}\n"
            "ssize_t pread(int fd, void *buf, size_t count, off_t offset) {\n"
            "    ssize_t (*real_pread)(int, void *, size_t, off_t) = dlsym(RTLD_NEXT, \"pread\");\n"
            "    ssize_t r = real_pread(fd, buf, count, offset);\n"
            "    maybe_corrupt(fd, buf, offset, r);\n"
            "    return r;\n"
            "}\n"
            "ssize_t pread64(int fd, void *buf, size_t count, off64_t offset) {\n"
            "    ssize_t (*real_pread64)(int, void *, size_t, off64_t) = dlsym(RTLD_NEXT, \"pread64\");\n"
            "    ssize_t r = real_pread64(fd, buf, count, offset);\n"
            "    maybe_corrupt(fd, buf, (off_t)offset, r);\n"
            "    return r;\n"
            "}\n",
            encoding="utf-8",
        )
        subprocess.run(
            ["gcc", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", str(corrupt_c), "-o", str(corrupt_so), "-ldl"],
            check=True,
        )
        swu = self._pack_swu("readback-corrupt.swu", target_slot=2)
        res = self._apply_swu(
            swu,
            self.slot2_img,
            expected_target=2,
            allow_virtual_slot2=True,
            extra_env={"LD_PRELOAD": str(corrupt_so)},
        )
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("slot readback digest mismatch", res.stderr)
        env = self._get_env()
        self.assertEqual(env["sgw_active_slot"], "1")
        self.assertEqual(env["sgw_last_good_slot"], "1")
        self.assertEqual(env["sgw_pending_slot"], "none")
        self.assertEqual(env["sgw_ab_state"], "confirmed")
        self.assertEqual(env["upgrade_available"], "0")
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

    def test_rollback_reconciles_stale_pending_and_accepts_next_valid_swu(self):
        stale_id = "c" * 64
        stale_txn = self.txdir / f"txn-{stale_id}"
        stale_mf = self.txdir / f"txn-{stale_id}.manifest"
        stale_txn.write_text(
            f"SGW-TXN-1\n"
            f"state=PENDING\n"
            f"id={stale_id}\n"
            f"package={'d' * 64}\n"
            f"image={'e' * 64}\n"
            f"rootfs_manifest={'f' * 64}\n"
            f"target=2\n"
            f"last_good=1\n"
            f"written_size=8388608\n"
            f"consumed=0\n",
            encoding="utf-8",
        )
        stale_mf.write_text("/sbin/init " + ("a" * 64) + "\n", encoding="utf-8")
        stale_txn.chmod(0o600)
        stale_mf.chmod(0o600)

        swu = self._pack_swu("next-valid-slot2.swu", target_slot=2)

        # While U-Boot env is still confirmed (no rollback happened), stale PENDING must block!
        r_blocked = self._apply_swu(swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertNotEqual(r_blocked.returncode, 0)
        self.assertIn("state=PENDING", stale_txn.read_text(encoding="utf-8"))

        # Now simulate U-Boot altbootcmd rollback back to slot 1
        self._set_env({
            "sgw_active_slot": "1",
            "sgw_last_good_slot": "1",
            "sgw_pending_slot": "none",
            "sgw_ab_state": "rolled_back",
            "sgw_last_error": "bootlimit_exceeded",
            "sgw_rollback_count": "1",
            "upgrade_available": "0",
            "bootcount": "0",
        })
        r_ok = self._apply_swu(swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertEqual(r_ok.returncode, 0, r_ok.stdout + r_ok.stderr)
        m = re.search(r"STAGED tx=([0-9a-f]{64}) target=2 last_good=1", r_ok.stdout)
        self.assertIsNotNone(m, r_ok.stdout + r_ok.stderr)
        new_id = m.group(1)
        self.assertNotEqual(new_id, stale_id)

        old_parsed = self._parse_txn(stale_id)
        self.assertEqual(old_parsed["state"], "ROLLED_BACK")
        self.assertEqual(old_parsed["consumed"], "1")
        self.assertNotIn("bootcount", old_parsed)
        if Path("/proc/sys/kernel/random/boot_id").exists():
            self.assertIn("boot_id", old_parsed)

        # Also verify a legacy pre-settled ROLLED_BACK record with boot_id + bootcount=0
        # (such as written by an earlier watchdog_guard before omitting bootcount=0)
        # does not block subsequent valid SWU installs when the board is on rolled_back p1.
        for p in self.txdir.glob("txn-*"):
            p.unlink()
        legacy_id = "b" * 64
        legacy_txn = self.txdir / f"txn-{legacy_id}"
        legacy_mf = self.txdir / f"txn-{legacy_id}.manifest"
        legacy_txn.write_text(
            f"SGW-TXN-1\n"
            f"state=ROLLED_BACK\n"
            f"id={legacy_id}\n"
            f"package={'d' * 64}\n"
            f"image={'e' * 64}\n"
            f"rootfs_manifest={'f' * 64}\n"
            f"target=2\n"
            f"last_good=1\n"
            f"written_size=8388608\n"
            f"consumed=1\n"
            f"boot_id=6b31ed54-2faf-42fe-a98c-54011f11a81a\n"
            f"bootcount=0\n",
            encoding="utf-8",
        )
        legacy_mf.write_text("/sbin/init " + ("a" * 64) + "\n", encoding="utf-8")
        legacy_txn.chmod(0o600)
        legacy_mf.chmod(0o600)
        self._set_env({
            "sgw_active_slot": "1",
            "sgw_last_good_slot": "1",
            "sgw_pending_slot": "none",
            "sgw_ab_state": "rolled_back",
            "sgw_last_error": "bootlimit_exceeded",
            "sgw_rollback_count": "2",
            "upgrade_available": "0",
            "bootcount": "0",
        })
        r_legacy_ok = self._apply_swu(swu, self.slot2_img, expected_target=2, allow_virtual_slot2=True)
        self.assertEqual(r_legacy_ok.returncode, 0, r_legacy_ok.stdout + r_legacy_ok.stderr)
        m2 = re.search(r"STAGED tx=([0-9a-f]{64}) target=2 last_good=1", r_legacy_ok.stdout)
        self.assertIsNotNone(m2, r_legacy_ok.stdout + r_legacy_ok.stderr)
        new_id = m2.group(1)
        legacy_parsed = self._parse_txn(legacy_id)
        self.assertEqual(legacy_parsed["state"], "ROLLED_BACK")
        self.assertEqual(legacy_parsed["consumed"], "1")

        new_parsed = self._parse_txn(new_id)
        self.assertEqual(new_parsed["state"], "PENDING")
        self.assertEqual(new_parsed["consumed"], "0")
        self.assertEqual(new_parsed["target"], "2")
        self.assertEqual(new_parsed["last_good"], "1")

        env = self._get_env()
        self.assertEqual(env["sgw_ab_state"], "trial")
        self.assertEqual(env["sgw_pending_slot"], "2")
        self.assertEqual(env["sgw_last_good_slot"], "1")
        self.assertEqual(env["upgrade_available"], "1")


if __name__ == "__main__":
    unittest.main()
