#!/usr/bin/env python3
import json
import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
UBOOT_UPSTREAM = Path(os.environ.get("UBOOT_SRC_DIR", str(Path.home() / "Desktop/Uboot-2017.03")))
UBOOT_BUILD = Path(os.environ.get("UBOOT_AB_BUILD_DIR", str(REPO_ROOT / "build/uboot-ab")))
HOST_FW_PRINTENV = UBOOT_BUILD / "host/fw_printenv"
HOST_FW_SETENV = UBOOT_BUILD / "host/fw_setenv"
BOOTCTL_BIN = REPO_ROOT / "bsp/apps/boot_ab/sgw_bootctl"

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


class TestG2UbootAB(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not HOST_FW_PRINTENV.exists():
            raise RuntimeError(f"Missing {HOST_FW_PRINTENV}; run bsp/uboot/build_uboot_ab.sh first")
        if not BOOTCTL_BIN.exists():
            raise RuntimeError(f"Missing {BOOTCTL_BIN}; run make -C bsp/apps/boot_ab first")

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
        self.env = os.environ.copy()
        self.env["SGW_FW_PRINTENV"] = str(HOST_FW_PRINTENV)
        self.env["SGW_FW_SETENV"] = str(HOST_FW_SETENV)
        self.env["SGW_FW_ENV_CONFIG"] = str(self.cfg_path)

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

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

    def run_bootctl(self, *args, check=True):
        return subprocess.run(
            [str(BOOTCTL_BIN), "-c", str(self.cfg_path), *args],
            env=self.env,
            capture_output=True,
            text=True,
            check=check,
        )

    def simulate_uboot_boot(self) -> tuple:
        """Simulate U-Boot 2017.03 bootcount_env + autoboot altbootcmd + ab_select_slot
        directly against the simulated eMMC redundant env using real fw_printenv/fw_setenv.
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
            # Mirrors altbootcmd in mx6ullevk.h
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

        # Mirrors ab_select_slot in mx6ullevk.h
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

    def test_uboot_2017_03_backends_and_compiled_symbols(self):
        bootcount_dir = UBOOT_UPSTREAM / "drivers/bootcount"
        self.assertTrue((bootcount_dir / "bootcount_env.c").exists())
        self.assertTrue((bootcount_dir / "bootcount_ram.c").exists())
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
            "#define CONFIG_CMD_EXT4 1",
            "#define CONFIG_CMD_SETEXPR 1",
        ):
            self.assertIn(macro, cfg_text)

        sym_text = (UBOOT_BUILD / "arm/System.map").read_text(encoding="utf-8")
        for sym in ("bootcount_load", "bootcount_store", "env_relocate_spec", "hw_watchdog_init", "hw_watchdog_reset"):
            self.assertRegex(sym_text, rf"\b{sym}\b")

        self.assertTrue((UBOOT_BUILD / "arm/u-boot-dtb.imx").exists())
        self.assertTrue((UBOOT_BUILD / "arm/fw_printenv").exists())

        fw_cfg = (REPO_ROOT / "bsp/uboot/fw_env.ab.config").read_text(encoding="utf-8")
        self.assertIn("/dev/mmcblk1  0xe0000  0x2000", fw_cfg)
        self.assertIn("/dev/mmcblk1  0xe2000  0x2000", fw_cfg)
        self.assertLess(ENV_OFFSET_1 + ENV_SIZE, 4098 * 512)

    def test_default_env_preserves_p2_and_swupdate_var(self):
        # Seed redundant env cleanly with real fw_setenv (write twice so both redundant copies are populated)
        self.run_fw_setenv_batch({"sgw_last_error": "none"})
        self.run_fw_setenv_batch({"sgw_active_slot": "2"})

        st = json.loads(self.run_bootctl("status", "--json").stdout)
        self.assertTrue(st["valid"])
        self.assertEqual(st["sgw_active_slot"], "2")
        self.assertEqual(st["device"], "/dev/mmcblk1p2")
        self.assertEqual(st["sgw_last_good_slot"], "2")
        self.assertEqual(st["sgw_ab_state"], "confirmed")
        self.assertEqual(st["bootlimit"], 3)
        self.assertEqual(st["bootcount"], 0)
        self.assertEqual(st["upgrade_available"], 0)
        self.assertEqual(st["swu_update"], "yes")

        img = self.img_path.read_bytes()
        crc0_stored, crc0_calc, flag0 = read_redund_copy_info(img, ENV_OFFSET_0)
        crc1_stored, crc1_calc, flag1 = read_redund_copy_info(img, ENV_OFFSET_1)
        self.assertEqual(crc0_stored, crc0_calc)
        self.assertEqual(crc1_stored, crc1_calc)
        self.assertNotEqual(flag0, flag1)

    def test_trial_bootlimit_rollback_via_real_fw_env(self):
        self.run_fw_setenv_batch({"recovery_status": "idle_sentinel", "sgw_last_error": "none"})
        res = self.run_bootctl("stage", "--target", "1", "--running", "2")
        self.assertIn("STAGED target=1", res.stdout)

        st = json.loads(self.run_bootctl("status", "--json").stdout)
        self.assertEqual(st["sgw_active_slot"], "1")
        self.assertEqual(st["sgw_pending_slot"], "1")
        self.assertEqual(st["sgw_ab_state"], "trial")
        self.assertEqual(st["upgrade_available"], 1)
        self.assertEqual(st["swu_update"], "yes")
        self.assertEqual(st["swu_recovery_status"], "idle_sentinel")

        # 3 failed boots on p1
        for expected_bc in (1, 2, 3):
            slot, rolled, bc = self.simulate_uboot_boot()
            self.assertEqual(slot, "1")
            self.assertFalse(rolled)
            self.assertEqual(bc, expected_bc)

        # Boot 4 exceeds bootlimit=3 -> altbootcmd rolls back to p2
        slot, rolled, bc = self.simulate_uboot_boot()
        self.assertEqual(slot, "2")
        self.assertTrue(rolled)
        self.assertEqual(bc, 0)

        st = json.loads(self.run_bootctl("status", "--json").stdout)
        self.assertEqual(st["sgw_active_slot"], "2")
        self.assertEqual(st["device"], "/dev/mmcblk1p2")
        self.assertEqual(st["sgw_last_good_slot"], "2")
        self.assertEqual(st["sgw_pending_slot"], "none")
        self.assertEqual(st["sgw_ab_state"], "rolled_back")
        self.assertEqual(st["sgw_rollback_count"], 1)
        self.assertEqual(st["sgw_last_error"], "bootlimit_exceeded")
        self.assertEqual(st["upgrade_available"], 0)
        self.assertEqual(st["swu_update"], "yes")
        self.assertEqual(st["swu_recovery_status"], "idle_sentinel")

    def test_bidirectional_p1_p2_confirm(self):
        self.run_fw_setenv_batch({"sgw_last_error": "none"})
        self.run_bootctl("stage", "--target", "1", "--running", "2")
        slot, rolled, bc = self.simulate_uboot_boot()
        self.assertEqual((slot, rolled, bc), ("1", False, 1))

        # Unhealthy check refuses commit
        bad = self.run_bootctl("confirm", "--running", "1", "--health-ok", "0", check=False)
        self.assertNotEqual(bad.returncode, 0)
        # Wrong slot refuses commit
        wrong = self.run_bootctl("confirm", "--running", "2", "--health-ok", "1", check=False)
        self.assertNotEqual(wrong.returncode, 0)

        # Commit p1
        ok = self.run_bootctl("confirm", "--running", "1", "--health-ok", "1")
        self.assertIn("CONFIRMED active=1 last_good=1 state=confirmed", ok.stdout)

        # Switch back p1 -> p2
        self.run_bootctl("stage", "--target", "2", "--running", "1")
        slot, rolled, bc = self.simulate_uboot_boot()
        self.assertEqual((slot, rolled, bc), ("2", False, 1))
        ok2 = self.run_bootctl("confirm", "--running", "2", "--health-ok", "1")
        self.assertIn("CONFIRMED active=2 last_good=2 state=confirmed", ok2.stdout)

    def test_variable_collision_and_preflight_guards(self):
        self.run_fw_setenv_batch({"sgw_last_error": "none"})

        same = self.run_bootctl("stage", "--target", "2", "--running", "2", check=False)
        self.assertNotEqual(same.returncode, 0)
        self.assertIn("refusing to stage currently running slot", same.stderr)

        self.run_fw_setenv_batch({"recovery_status": "in_progress"})
        swu_busy = self.run_bootctl("stage", "--target", "1", "--running", "2", check=False)
        self.assertNotEqual(swu_busy.returncode, 0)
        self.assertIn("recovery_status=in_progress", swu_busy.stderr)

    def test_redundant_env_powerloss_corruption_recovery(self):
        self.run_fw_setenv_batch({"sgw_last_error": "none"})
        self.run_fw_setenv_batch({"sgw_active_slot": "2"})
        self.run_bootctl("stage", "--target", "1", "--running", "2")
        # Copy before corruption has confirmed or older state; simulate 1 more write so
        # one copy holds confirmed p1 or older state and newer holds trial state, then corrupt newer.
        img = bytearray(self.img_path.read_bytes())
        _, _, flag0 = read_redund_copy_info(bytes(img), ENV_OFFSET_0)
        _, _, flag1 = read_redund_copy_info(bytes(img), ENV_OFFSET_1)
        # In U-Boot fw_env, flag wraps 255 -> 1 -> 2 -> 3 ...
        newer_offset = ENV_OFFSET_0 if flag0 > flag1 else ENV_OFFSET_1
        img[newer_offset:newer_offset + 128] = b"\x00" * 128
        self.img_path.write_bytes(bytes(img))

        # Surviving redundant copy still validates cleanly with valid CRC
        res = self.run_bootctl("validate")
        self.assertEqual(res.returncode, 0)
        self.assertIn("VALID", res.stdout)

    def test_watchdog_trial_expiry_cli(self):
        r0 = self.run_bootctl("watchdog-check", "--elapsed-ms", "59999", "--budget-ms", "60000")
        self.assertEqual(r0.returncode, 0)
        self.assertIn("expired=0", r0.stdout)

        r1 = self.run_bootctl("watchdog-check", "--elapsed-ms", "60000", "--budget-ms", "60000", check=False)
        self.assertEqual(r1.returncode, 3)
        self.assertIn("expired=1", r1.stdout)

        r2 = self.run_bootctl(
            "watchdog-check",
            "--elapsed-ms",
            "120000",
            "--budget-ms",
            "60000",
            "--health-confirmed",
            "1",
        )
        self.assertEqual(r2.returncode, 0)
        self.assertIn("expired=0", r2.stdout)


if __name__ == "__main__":
    unittest.main()
