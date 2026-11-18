#!/usr/bin/env python3
"""G4 isolated virtual-disk SWUpdate (.swu) end-to-end verification.

Covers on virtual slot1/slot2/data/emmc_env images (never touching /dev/mmcblk1):
1. Positive p1 -> p2: RSA-2048 .swu verify -> target guard -> write + fsync + readback -> PENDING TXN & redundant U-Boot env.
2. Positive p2 -> p1: reverse direction from p2 -> p1, keeping p2 untouched.
3. Negative rejections (fail-closed with ZERO mutation to slot images and U-Boot env):
   - bad / untrusted signature
   - corrupted rootfs.img payload
   - corrupted zImage payload
   - wrong board identifier
   - wrong target slot (target == running == last_good)
   - slot capacity smaller than rootfs.img
   - path traversal / unsafe archive entry in .swu
"""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
TOOL = ROOT / "bsp/ota/g4_swu_tool.py"
HARNESS = ROOT / "bsp/apps/boot_ab/r3c_confirm_harness"
UBOOT_BUILD = Path(os.environ.get("UBOOT_AB_BUILD_DIR", str(ROOT / "build/uboot-ab")))
FW_PRINTENV = UBOOT_BUILD / "host/fw_printenv"
FW_SETENV = UBOOT_BUILD / "host/fw_setenv"
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
        for p in (TOOL, HARNESS, FW_PRINTENV, FW_SETENV):
            if not p.exists():
                raise RuntimeError(f"missing required artifact: {p}")

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".sgw-g4-swu-", dir=Path.home())
        self.addCleanup(self.tmp.cleanup)
        self.addCleanup(self._unmount)
        self.base = Path(self.tmp.name)
        self.base.chmod(0o700)

        # Redundant U-Boot env (4 MiB sparse image with 0xE0000 & 0xE2000)
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

        # Virtual slot1 (8 MiB), slot2 (16 MiB), and DATA (32 MiB via fuse2fs)
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
        subprocess.run([FUSE2FS, str(self.data_img), str(self.data_mnt)], check=True, capture_output=True, timeout=10)
        self.mounted = True
        self.data_mnt.chmod(0o700)
        schema = self.data_mnt / ".sgw-data-schema"
        schema.write_text("schema=1\n", encoding="utf-8")
        schema.chmod(0o600)
        self.txdir = self.data_mnt / ".sgw-tx"
        self.txdir.mkdir(mode=0o700)

        # Fake OS proc/sysroot context (default running slot = 1)
        self.sysroot = self.base / "sysroot"
        self.sysroot.mkdir(mode=0o755)
        self.cmdline = self.base / "cmdline"
        self.mounts = self.base / "mounts"
        self.mountinfo = self.base / "mountinfo"
        self._set_running_slot(1)

        # RSA-2048 keypair + payload ext4 image
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

    def _pack_swu(self, out_name: str, target_slot: int, board: str = "imx6ull-100ask", privkey: Path = None) -> Path:
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
                "--out", str(swu),
            ],
            check=True, capture_output=True, text=True,
        )
        return swu

    def _apply_swu(self, swu: Path, slot_img: Path, expected_target=None):
        cmd = [
            "python3", str(TOOL), "apply",
            "--swu", str(swu),
            "--pubkey", str(self.pubkey),
            "--slot-img", str(slot_img),
            "--txdir", str(self.txdir),
            "--fw-cfg", str(self.fw_cfg),
            "--fw-printenv", str(FW_PRINTENV),
            "--fw-setenv", str(FW_SETENV),
            "--cmdline", str(self.cmdline),
            "--mounts", str(self.mounts),
            "--mountinfo", str(self.mountinfo),
            "--sysroot", str(self.sysroot),
        ]
        if expected_target is not None:
            cmd.extend(["--expected-target", str(expected_target)])
        return subprocess.run(cmd, capture_output=True, text=True, timeout=180)

    def test_p1_to_p2_signed_swu_verify_write_readback_and_pending_stage(self):
        slot1_before = sha256_path(self.slot1_img)
        payload_sha = sha256_path(self.payload_ext4)
        swu = self._pack_swu("g4-to-p2.swu", target_slot=2)
        res = self._apply_swu(swu, self.slot2_img, expected_target=2)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("STAGED tx=", res.stdout)
        txid = res.stdout.split("tx=")[1].split()[0]

        # Readback written virtual slot2 matches payload byte-for-byte; running slot1 is untouched
        written_prefix = self.slot2_img.read_bytes()[: self.payload_ext4.stat().st_size]
        self.assertEqual(hashlib.sha256(written_prefix).hexdigest(), payload_sha)
        self.assertEqual(sha256_path(self.slot1_img), slot1_before)

        # Redundant U-Boot env staged for trial boot on slot 2 while last_good stays 1
        env = self._get_env()
        self.assertEqual(env["sgw_pending_slot"], "2")
        self.assertEqual(env["sgw_last_good_slot"], "1")
        self.assertEqual(env["sgw_ab_state"], "trial")
        self.assertEqual(env["upgrade_available"], "1")
        self.assertEqual(env["bootcount"], "0")

        # Inspect persisted PENDING transaction
        ins = subprocess.run([str(HARNESS), "inspect-txn", "--txdir", str(self.txdir), "--id", txid],
                             check=True, capture_output=True, text=True)
        self.assertIn("state=PENDING", ins.stdout)
        self.assertIn("target=2", ins.stdout)
        self.assertIn("last_good=1", ins.stdout)
        self.assertIn("consumed=0", ins.stdout)

    def test_p2_to_p1_signed_swu_reverse_direction(self):
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
        res = self._apply_swu(swu, self.slot1_img, expected_target=1)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertEqual(sha256_path(self.slot1_img), payload_sha)
        self.assertEqual(sha256_path(self.slot2_img), slot2_before)
        env = self._get_env()
        self.assertEqual(env["sgw_pending_slot"], "1")
        self.assertEqual(env["sgw_last_good_slot"], "2")

    def test_reject_bad_signature_corrupt_payload_wrong_board_wrong_slot_and_capacity(self):
        s1_before = sha256_path(self.slot1_img)
        s2_before = sha256_path(self.slot2_img)
        env_before = sha256_path(self.emmc_img)

        # 1. Untrusted signing key -> rejected before touching virtual disk
        bad_priv = self.base / "untrusted.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                        "-out", str(bad_priv)], check=True, capture_output=True)
        bad_sig_swu = self._pack_swu("bad-key.swu", target_slot=2, privkey=bad_priv)
        r = self._apply_swu(bad_sig_swu, self.slot2_img, expected_target=2)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("signature verification failed", r.stderr)

        # 2. Corrupt rootfs.img inside signed .swu -> rejected by payload digest check
        good_swu = self._pack_swu("good-for-tamper.swu", target_slot=2)
        tamper_dir = self.base / "tamper"
        tamper_dir.mkdir()
        subprocess.run(["cpio", "-iu", "--quiet"], cwd=str(tamper_dir), input=good_swu.read_bytes(), check=True)
        with (tamper_dir / "rootfs.img").open("r+b") as f:
            f.seek(4096)
            b = f.read(1)
            f.seek(4096)
            f.write(bytes([b[0] ^ 0xFF]))
        tamper_swu = self.base / "tamper-rootfs.swu"
        entries = ["sw-description", "sw-description.sig", "manifest", "manifest.sig",
                   "rootfs.manifest", "zImage", "100ask_imx6ull-14x14.dtb", "rootfs.img"]
        with tamper_swu.open("wb") as out_f:
            subprocess.run(["cpio", "-o", "-H", "newc", "--quiet"], cwd=str(tamper_dir),
                           input=("\n".join(entries) + "\n").encode(), stdout=out_f, check=True)
        r2 = self._apply_swu(tamper_swu, self.slot2_img, expected_target=2)
        self.assertNotEqual(r2.returncode, 0)
        self.assertIn("digest/size mismatch", r2.stderr)

        # 3. Corrupt zImage inside signed .swu -> rejected
        tamper_z_dir = self.base / "tamper_z"
        tamper_z_dir.mkdir()
        subprocess.run(["cpio", "-iu", "--quiet"], cwd=str(tamper_z_dir), input=good_swu.read_bytes(), check=True)
        (tamper_z_dir / "zImage").write_bytes(b"corrupted-zimage\n")
        tamper_z_swu = self.base / "tamper-zimage.swu"
        with tamper_z_swu.open("wb") as out_f:
            subprocess.run(["cpio", "-o", "-H", "newc", "--quiet"], cwd=str(tamper_z_dir),
                           input=("\n".join(entries) + "\n").encode(), stdout=out_f, check=True)
        r3 = self._apply_swu(tamper_z_swu, self.slot2_img, expected_target=2)
        self.assertNotEqual(r3.returncode, 0)
        self.assertIn("digest/size mismatch for zImage", r3.stderr)

        # 4. Wrong board -> rejected
        wrong_board_swu = self._pack_swu("wrong-board.swu", target_slot=2, board="other-board")
        r4 = self._apply_swu(wrong_board_swu, self.slot2_img, expected_target=2)
        self.assertNotEqual(r4.returncode, 0)
        self.assertIn("board mismatch", r4.stderr)

        # 5. Wrong target slot (targeting currently running & last_good slot 1!) -> refused
        wrong_slot_swu = self._pack_swu("wrong-slot.swu", target_slot=1)
        r5 = self._apply_swu(wrong_slot_swu, self.slot1_img)
        self.assertNotEqual(r5.returncode, 0)
        self.assertIn("REFUSED: target_slot 1", r5.stderr)

        # 6. Slot capacity too small -> refused
        small_slot = self.base / "small_slot.ext4"
        with small_slot.open("wb") as f:
            f.truncate(2 * 1024 * 1024)
        small_sha = sha256_path(small_slot)
        r6 = self._apply_swu(good_swu, small_slot, expected_target=2)
        self.assertNotEqual(r6.returncode, 0)
        self.assertIn("capacity", r6.stderr)
        self.assertEqual(sha256_path(small_slot), small_sha)

        # Verify ZERO mutation across all negative tests
        self.assertEqual(sha256_path(self.slot1_img), s1_before)
        self.assertEqual(sha256_path(self.slot2_img), s2_before)
        self.assertEqual(sha256_path(self.emmc_img), env_before)
        self.assertEqual(list(self.txdir.glob("txn-*")), [])

    def test_reject_unsafe_cpio_path_traversal(self):
        trav_dir = self.base / "trav"
        (trav_dir / "sub").mkdir(parents=True)
        (trav_dir / "sub/evil").write_text("x\n", encoding="utf-8")
        trav_swu = self.base / "trav.swu"
        with trav_swu.open("wb") as out_f:
            subprocess.run(["cpio", "-o", "-H", "newc", "--quiet"], cwd=str(trav_dir),
                           input=b"sub/evil\n", stdout=out_f, check=True)
        r = self._apply_swu(trav_swu, self.slot2_img)
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("unsafe archive member path", r.stderr)


    def test_real_g3_artifacts_swu_p1_to_p2_and_pending_stage(self):
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
        res = self._apply_swu(real_swu, virt_p2, expected_target=2)
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

if __name__ == "__main__":
    unittest.main()
