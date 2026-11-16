#!/usr/bin/env python3
import json
import os
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
UBOOT_UPSTREAM = Path(os.environ.get("UBOOT_SRC_DIR", str(Path.home() / "Desktop/Uboot-2017.03")))
UBOOT_BUILD = Path(os.environ.get("UBOOT_AB_BUILD_DIR", str(REPO_ROOT / "build/uboot-ab")))
HOST_FW_PRINTENV = UBOOT_BUILD / "host/fw_printenv"
HOST_FW_SETENV = UBOOT_BUILD / "host/fw_setenv"
SINGLE_FW_PRINTENV = UBOOT_BUILD / "host-single/fw_printenv"
SINGLE_FW_SETENV = UBOOT_BUILD / "host-single/fw_setenv"
MIGRATE_SCRIPT = REPO_ROOT / "bsp/uboot/migrate_single_to_redund_env.sh"
BOOTCTL_BIN = REPO_ROOT / "bsp/apps/boot_ab/sgw_bootctl"
WDT_GUARD_BIN = REPO_ROOT / "bsp/apps/boot_ab/sgw_watchdog_guard"

ENV_OFFSET_0 = 0xE0000
ENV_OFFSET_1 = 0xE2000
ENV_SIZE = 0x2000


def parse_fw_printenv(output: str) -> dict:
    kv = {}
    for line in output.splitlines():
        if "=" in line:
            k, v = line.split("=", 1)
            kv[k] = v
    return kv


def read_redund_copy_info(img_bytes: bytes, offset: int) -> tuple:
    block = img_bytes[offset:offset + ENV_SIZE]
    stored_crc, flag = struct.unpack("<IB", block[:5])
    calc_crc = zlib.crc32(block[5:]) & 0xFFFFFFFF
    return stored_crc, calc_crc, flag


class MockSupervisorServer:
    """Minimal real UNIX domain socket server implementing common/service_ipc.h."""

    def __init__(self, run_dir: Path, pid: int, starttime: int = 99999):
        self.run_dir = run_dir
        self.sock_path = run_dir / "supervisor.sock"
        self.pid = pid
        self.starttime = starttime
        self.healthy = True
        self._stop = False
        self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self._sock.bind(str(self.sock_path))
        self._sock.listen(8)
        self._sock.settimeout(0.05)
        self._thread = threading.Thread(target=self._serve, daemon=True)
        self._thread.start()

    def _serve(self):
        while not self._stop:
            try:
                conn, _ = self._sock.accept()
            except socket.timeout:
                continue
            except OSError:
                break
            with conn:
                data = conn.recv(256).decode("utf-8", errors="ignore").strip()
                if data == "STATUS":
                    if self.healthy:
                        msg = f"OK {self.pid} {self.starttime} gateway alive\n"
                    else:
                        msg = "NOT_RUNNING 0 0 gateway stopped\n"
                else:
                    msg = "BAD_REQUEST 0 0 unknown\n"
                conn.sendall(msg.encode("utf-8"))

    def close(self):
        self._stop = True
        self._sock.close()
        self._thread.join(timeout=1.0)


class TestG2UbootAB(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        for p in (HOST_FW_PRINTENV, SINGLE_FW_PRINTENV, BOOTCTL_BIN, WDT_GUARD_BIN):
            if not p.exists():
                raise RuntimeError(f"Missing {p}; build bsp/uboot & bsp/apps/boot_ab first")

    def setUp(self):
        self.tmpdir = Path(tempfile.mkdtemp(prefix="sgw_g2_test_"))
        self.img_path = self.tmpdir / "emmc.img"
        self.cfg_path = self.tmpdir / "fw_env.config"
        with open(self.img_path, "wb") as f:
            f.truncate(4 * 1024 * 1024)
        self.cfg_path.write_text(
            f"{self.img_path} 0x{ENV_OFFSET_0:x} 0x{ENV_SIZE:x}\n"
            f"{self.img_path} 0x{ENV_OFFSET_1:x} 0x{ENV_SIZE:x}\n",
            encoding="utf-8",
        )

        # Synthetic OS runtime environment for trusted slot & health verification
        self.run_dir = self.tmpdir / "run"
        self.run_dir.mkdir()
        self.sysroot = self.tmpdir / "sysroot"
        (self.sysroot / "sbin").mkdir(parents=True)
        (self.sysroot / "etc").mkdir(parents=True)
        (self.sysroot / "sbin/init").write_text("init\n", encoding="utf-8")
        (self.sysroot / "etc/inittab").write_text("::sysinit:/etc/init.d/rcS\n", encoding="utf-8")
        self.cmdline_path = self.tmpdir / "cmdline"
        self.mounts_path = self.tmpdir / "mounts"
        self.set_os_running_slot("2")

        # Dummy live child representing managed gateway service PID
        self.dummy_proc = subprocess.Popen(["sleep", "30"])
        self.supervisor = MockSupervisorServer(self.run_dir, pid=self.dummy_proc.pid)

        self.env = os.environ.copy()
        self.env["SGW_FW_PRINTENV"] = str(HOST_FW_PRINTENV)
        self.env["SGW_FW_SETENV"] = str(HOST_FW_SETENV)
        self.env["SGW_FW_ENV_CONFIG"] = str(self.cfg_path)

    def tearDown(self):
        self.supervisor.close()
        self.dummy_proc.terminate()
        self.dummy_proc.wait(timeout=2)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def set_os_running_slot(self, slot: str, mount_opts: str = "rw,relatime"):
        self.cmdline_path.write_text(
            f"console=ttymxc0,115200 root=/dev/mmcblk1p{slot} rootwait rw\n",
            encoding="utf-8",
        )
        self.mounts_path.write_text(
            f"/dev/root / ext4 {mount_opts} 0 0\n",
            encoding="utf-8",
        )

    def run_fw_printenv(self) -> dict:
        res = subprocess.run(
            [str(HOST_FW_PRINTENV), "-c", str(self.cfg_path)],
            capture_output=True,
            text=True,
            check=True,
        )
        return parse_fw_printenv(res.stdout)

    def run_fw_setenv_batch(self, pairs: dict):
        script = "".join(f"{k} {v}\n" for k, v in pairs.items())
        subprocess.run(
            [str(HOST_FW_SETENV), "-c", str(self.cfg_path), "-s", "-"],
            input=script,
            capture_output=True,
            text=True,
            check=True,
        )

    def run_bootctl(self, *args, with_os_ctx: bool = True, check=True):
        cmd = [str(BOOTCTL_BIN), "-c", str(self.cfg_path)]
        if with_os_ctx:
            cmd.extend([
                "--run-dir", str(self.run_dir),
                "--cmdline", str(self.cmdline_path),
                "--mounts", str(self.mounts_path),
                "--sysroot", str(self.sysroot),
            ])
        cmd.extend(args)
        return subprocess.run(
            cmd,
            env=self.env,
            capture_output=True,
            text=True,
            check=check,
        )

    def simulate_uboot_boot(self) -> tuple:
        """Model U-Boot autoboot.c + bootcount_env.c + compiled ab_select_slot/altbootcmd
        against the binary redundant env image via real fw_printenv/fw_setenv.
        Note: Real hardware U-Boot UART execution remains BOARD-PENDING (G2->hardware gate).
        """
        kv = self.run_fw_printenv()
        upgrade_available = int(kv.get("upgrade_available", "0"))
        bootlimit = int(kv.get("bootlimit", "3"))
        bootcount = int(kv.get("bootcount", "0")) if upgrade_available else 0

        bootcount += 1
        rolled_back = False

        if upgrade_available:
            self.run_fw_setenv_batch({"bootcount": str(bootcount)})
            kv["bootcount"] = str(bootcount)

        if bootlimit > 0 and bootcount > bootlimit:
            rolled_back = True
            safe = kv.get("sgw_last_good_slot", "2")
            if safe not in ("1", "2"):
                safe = "2"
            rb_count = int(kv.get("sgw_rollback_count", "0")) + 1
            self.run_fw_setenv_batch({
                "sgw_active_slot": safe,
                "sgw_last_good_slot": safe,
                "sgw_pending_slot": "none",
                "sgw_ab_state": "rolled_back",
                "sgw_last_error": "bootlimit_exceeded",
                "sgw_rollback_count": str(rb_count),
                "upgrade_available": "0",
                "bootcount": "0",
            })
            kv = self.run_fw_printenv()

        if kv.get("upgrade_available") == "1" and kv.get("sgw_pending_slot") in ("1", "2"):
            selected = kv["sgw_pending_slot"]
            if kv.get("sgw_active_slot") != selected or kv.get("sgw_ab_state") != "trial":
                self.run_fw_setenv_batch({
                    "sgw_active_slot": selected,
                    "sgw_ab_state": "trial",
                })
        elif kv.get("sgw_active_slot") in ("1", "2"):
            selected = kv["sgw_active_slot"]
        else:
            selected = "2"

        return selected, rolled_back, int(kv.get("bootcount", "0"))

    def test_uboot_2017_03_backends_and_compiled_scripts(self):
        bootcount_dir = UBOOT_UPSTREAM / "drivers/bootcount"
        self.assertTrue((bootcount_dir / "bootcount_env.c").exists())
        self.assertFalse((bootcount_dir / "Kconfig").exists())

        cfg_text = (UBOOT_BUILD / "arm/u-boot.cfg").read_text(encoding="utf-8")
        for macro in (
            "#define CONFIG_BOOTCOUNT_LIMIT",
            "#define CONFIG_BOOTCOUNT_ENV",
            "#define CONFIG_ENV_OFFSET (14 * SZ_64K)",
            "#define CONFIG_ENV_OFFSET_REDUND (CONFIG_ENV_OFFSET + CONFIG_ENV_SIZE)",
            "#define CONFIG_SYS_REDUNDAND_ENVIRONMENT",
            "#define CONFIG_HW_WATCHDOG",
            "#define CONFIG_IMX_WATCHDOG",
            "#define CONFIG_WATCHDOG_TIMEOUT_MSECS 60000",
        ):
            self.assertIn(macro, cfg_text)

        sym_text = (UBOOT_BUILD / "arm/System.map").read_text(encoding="utf-8")
        for sym in ("bootcount_load", "bootcount_store", "env_relocate_spec", "hw_watchdog_init", "hw_watchdog_reset"):
            self.assertRegex(sym_text, rf"\b{sym}\b")

        self.assertTrue((UBOOT_BUILD / "arm/u-boot-dtb.imx").exists())
        self.assertTrue((UBOOT_BUILD / "arm/fw_printenv").exists())
        self.assertTrue((REPO_ROOT / "build/arm-ab/sgw_bootctl").exists())
        self.assertTrue((REPO_ROOT / "build/arm-ab/sgw_watchdog_guard").exists())

        # Verify compiled default scripts emitted by U-Boot's own default_environment[]
        kv = self.run_fw_printenv()
        self.assertEqual(kv["sgw_active_slot"], "2")
        self.assertEqual(kv["sgw_last_good_slot"], "2")
        self.assertEqual(kv["update"], "yes")
        self.assertIn("run ab_select_slot;", kv["bootcmd"])
        self.assertIn("run ab_boot_fallback;", kv["bootcmd"])
        self.assertIn("panic=5", kv["mmcargs"])
        self.assertIn("reset;", kv["ab_boot_fallback"])
        self.assertIn("setexpr sgw_rollback_count", kv["altbootcmd"])
        self.assertIn("saveenv;", kv["altbootcmd"])

    def seed_simulated_trial(self):
        # File-backed fw_env simulation only; this does not stage a real slot.
        self.run_fw_setenv_batch({
            "sgw_active_slot": "1",
            "sgw_last_good_slot": "2",
            "sgw_pending_slot": "1",
            "sgw_ab_state": "trial",
            "bootcount": "0",
            "upgrade_available": "1",
        })

    def test_fail_closed_confirm_and_trusted_health_checks(self):
        self.run_fw_setenv_batch({"sgw_last_error": "none"})
        self.seed_simulated_trial()
        self.set_os_running_slot("1")
        before = self.run_fw_printenv()

        # A mock cmdline + mock Supervisor is not an actual mounted slot or
        # a trusted system/business health provider.
        result = self.run_bootctl("confirm", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("CONFIRM REJECTED", result.stderr)
        self.assertEqual(self.run_fw_printenv(), before)

        bypass = self.run_bootctl("confirm", "--health-ok", "1", check=False)
        self.assertNotEqual(bypass.returncode, 0)
        self.assertEqual(self.run_fw_printenv(), before)

        # stage is also rejected when actual root identity is not proven.
        stage = self.run_bootctl("stage", "--target", "2", check=False)
        self.assertNotEqual(stage.returncode, 0)
        self.assertEqual(self.run_fw_printenv(), before)

    def test_watchdog_guard_takeover_confirm_and_timeout_reset_path(self):
        self.run_fw_setenv_batch({"sgw_last_error": "none"})
        self.seed_simulated_trial()
        self.set_os_running_slot("1")
        # An apparently healthy mock process must not confirm an untrusted slot.
        self.supervisor.healthy = True
        wdt_file = self.tmpdir / "watchdog-file"
        wdt_file.touch()
        res = subprocess.run(
            [str(WDT_GUARD_BIN), "-c", str(self.cfg_path),
             "--wdt-dev", str(wdt_file), "--budget-ms", "250",
             "--poll-ms", "50", "--run-dir", str(self.run_dir),
             "--cmdline", str(self.cmdline_path), "--mounts", str(self.mounts_path),
             "--sysroot", str(self.sysroot), "--exit-on-confirm"],
            env=self.env, capture_output=True, text=True,
        )
        self.assertEqual(res.returncode, 3, res.stderr)
        self.assertIn("WATCHDOG_EXPIRED", res.stderr)
        self.assertNotIn(b"V", wdt_file.read_bytes())
        status = json.loads(self.run_bootctl("status", "--json").stdout)
        self.assertEqual(status["sgw_ab_state"], "trial")
        self.assertEqual(status["upgrade_available"], 1)

    def test_trial_bootlimit_rollback_and_swupdate_isolation(self):
        self.run_fw_setenv_batch({"recovery_status": "idle_sentinel", "sgw_last_error": "none"})
        self.seed_simulated_trial()

        for expected_bc in (1, 2, 3):
            slot, rolled, bc = self.simulate_uboot_boot()
            self.assertEqual((slot, rolled, bc), ("1", False, expected_bc))

        slot, rolled, bc = self.simulate_uboot_boot()
        self.assertEqual((slot, rolled, bc), ("2", True, 0))

        st = json.loads(self.run_bootctl("status", "--json").stdout)
        self.assertEqual(st["sgw_active_slot"], "2")
        self.assertEqual(st["sgw_last_good_slot"], "2")
        self.assertEqual(st["sgw_ab_state"], "rolled_back")
        self.assertEqual(st["sgw_rollback_count"], 1)
        self.assertEqual(st["sgw_last_error"], "bootlimit_exceeded")
        self.assertEqual(st["swu_update"], "yes")
        self.assertEqual(st["swu_recovery_status"], "idle_sentinel")

    def test_single_to_redundant_env_migration(self):
        single_img = self.tmpdir / "legacy_single.img"
        with open(single_img, "wb") as f:
            f.truncate(4 * 1024 * 1024)
        single_cfg = self.tmpdir / "single_fw_env.config"
        single_cfg.write_text(f"{single_img} 0x{ENV_OFFSET_0:x} 0x{ENV_SIZE:x}\n", encoding="utf-8")
        redund_cfg = self.tmpdir / "redund_fw_env.config"
        redund_cfg.write_text(
            f"{single_img} 0x{ENV_OFFSET_0:x} 0x{ENV_SIZE:x}\n"
            f"{single_img} 0x{ENV_OFFSET_1:x} 0x{ENV_SIZE:x}\n",
            encoding="utf-8",
        )

        # Seed legacy single-copy env (4B CRC header)
        subprocess.run(
            [str(SINGLE_FW_SETENV), "-c", str(single_cfg), "ethaddr", "aa:bb:cc:11:22:33"],
            capture_output=True, text=True, check=True,
        )
        subprocess.run(
            [str(SINGLE_FW_SETENV), "-c", str(single_cfg), "update", "yes"],
            capture_output=True, text=True, check=True,
        )

        # Redundant tool must NOT accept legacy 4B single-copy image without migration
        bad_read = subprocess.run(
            [str(HOST_FW_PRINTENV), "-c", str(redund_cfg)],
            capture_output=True, text=True, check=True,
        )
        self.assertIn("Bad CRC", bad_read.stderr)

        backup_txt = self.tmpdir / "legacy_backup.txt"
        mig = subprocess.run(
            [
                "bash", str(MIGRATE_SCRIPT),
                "--single-printenv", str(SINGLE_FW_PRINTENV),
                "--single-cfg", str(single_cfg),
                "--redund-setenv", str(HOST_FW_SETENV),
                "--redund-cfg", str(redund_cfg),
                "--backup-out", str(backup_txt),
            ],
            capture_output=True, text=True, check=True,
        )
        self.assertIn("completed cleanly", mig.stdout)
        self.assertIn("ethaddr=aa:bb:cc:11:22:33", backup_txt.read_text(encoding="utf-8"))

        # Verify BOTH redundant copies now pass 5B CRC32+flags and retain ethaddr + A/B p2 defaults
        img_bytes = single_img.read_bytes()
        crc0_s, crc0_c, fl0 = read_redund_copy_info(img_bytes, ENV_OFFSET_0)
        crc1_s, crc1_c, fl1 = read_redund_copy_info(img_bytes, ENV_OFFSET_1)
        self.assertEqual(crc0_s, crc0_c)
        self.assertEqual(crc1_s, crc1_c)
        self.assertNotEqual(fl0, fl1)

        after = subprocess.run(
            [str(HOST_FW_PRINTENV), "-c", str(redund_cfg)],
            capture_output=True, text=True, check=True,
        )
        self.assertEqual(after.stderr.strip(), "")
        kv_after = parse_fw_printenv(after.stdout)
        self.assertEqual(kv_after["ethaddr"], "aa:bb:cc:11:22:33")
        self.assertEqual(kv_after["update"], "yes")
        self.assertEqual(kv_after["sgw_active_slot"], "2")

    def test_redundant_env_powerloss_corruption_recovery(self):
        self.run_fw_setenv_batch({"sgw_last_error": "none"})
        self.run_fw_setenv_batch({"sgw_active_slot": "2"})
        self.seed_simulated_trial()
        img = bytearray(self.img_path.read_bytes())
        _, _, flag0 = read_redund_copy_info(bytes(img), ENV_OFFSET_0)
        _, _, flag1 = read_redund_copy_info(bytes(img), ENV_OFFSET_1)
        newer_offset = ENV_OFFSET_0 if flag0 > flag1 else ENV_OFFSET_1
        img[newer_offset:newer_offset + 128] = b"\x00" * 128
        self.img_path.write_bytes(bytes(img))

        res = self.run_bootctl("validate")
        self.assertEqual(res.returncode, 0)
        self.assertIn("VALID", res.stdout)

    def test_both_redundant_copies_corrupt_fails_closed(self):
        self.run_fw_setenv_batch({"sgw_last_error": "none"})
        self.run_fw_setenv_batch({"sgw_active_slot": "2"})
        img = bytearray(self.img_path.read_bytes())
        for offset in (ENV_OFFSET_0, ENV_OFFSET_1):
            img[offset:offset + 128] = b"\x00" * 128
        self.img_path.write_bytes(bytes(img))

        # fw_printenv 2017.03 may return zero while warning that it used
        # compiled defaults. The A/B controller must reject those defaults.
        result = self.run_bootctl("validate", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("state untrusted", result.stderr)
        result = self.run_bootctl("confirm", check=False)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(self.img_path.read_bytes(), bytes(img))


if __name__ == "__main__":
    unittest.main()
