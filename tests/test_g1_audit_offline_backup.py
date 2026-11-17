#!/usr/bin/env python3
"""Unit tests for g1_audit_offline_backup.py using synthetic backup layouts."""
import hashlib
import struct
import sys
import tempfile
import unittest
import zlib
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "bsp/tools"))
import g1_audit_offline_backup as audit


def make_env_block(pairs):
    payload = b"\x00".join(f"{k}={v}".encode("ascii") for k, v in pairs) + b"\x00\x00"
    payload = payload.ljust(audit.ENV_SIZE - 4, b"\x00")
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    return struct.pack("<I", crc) + payload


class OfflineAuditTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="g1-audit-")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        geometry = {"mmcblk1p1": (4098, 128), "mmcblk1p2": (4226, 64), "mmcblk1p3": (4290, 32)}
        for p in (
            patch.object(audit.source, "GEOMETRY", geometry),
            patch.object(audit.source, "PRE_SECTORS", 4098),
            patch.object(audit.source, "USER_SECTORS", 4354),
            patch.object(audit.source, "BOOT_SECTORS", 16),
        ):
            p.start()
            self.addCleanup(p.stop)

        tasks, _ = audit.verify_seg.layout()
        for name, size in tasks:
            (self.root / name).write_bytes(bytes(size))

        pre = bytearray((self.root / "emmc-reserved-pre-p1.bin").read_bytes())
        pre[446:510] = bytes(64)
        for i, ((_, (start, size)), typ) in enumerate(zip(geometry.items(), (0x83, 0x83, 0x0C))):
            pre[446 + 16 * i + 4] = typ
            struct.pack_into("<II", pre, 446 + 16 * i + 8, start, size)
        pre[510:512] = b"\x55\xaa"
        pre[0x400:0x404] = audit.IVT_MAGIC
        b1 = b"U-Boot 2017.03 (Jul 21 2024 - 05:57:43)"
        pre[0x500:0x500 + len(b1)] = b1
        pre[0xC0000:0xC0000 + audit.ENV_SIZE] = make_env_block([("baudrate", "115200")])
        pre[0xE0000:0xE0000 + audit.ENV_SIZE] = make_env_block([
            ("mmcdev", "1"), ("mmcpart", "2"), ("mmcroot", "/dev/mmcblk1p2 rootwait rw"),
            ("update", "yes"), ("panel", "TFT7016"), ("ethaddr", "00:01:1f:2d:3e:4d"),
        ])
        (self.root / "emmc-reserved-pre-p1.bin").write_bytes(pre)

        boot0 = bytearray((self.root / "emmc-boot0.img").read_bytes())
        boot0[0x400:0x404] = audit.IVT_MAGIC
        b0 = b"U-Boot 2017.03-g8ba4c5bb19-dirty (Jun 10 2026 - 10:24:59)"
        boot0[0x500:0x500 + len(b0)] = b0
        (self.root / "emmc-boot0.img").write_bytes(boot0)

        p2 = bytearray((self.root / "emmc-p2.img").read_bytes())
        struct.pack_into("<II", p2, 1024, 1000, 4096)
        struct.pack_into("<II", p2, 1024 + 12, 1000, 500)
        struct.pack_into("<I", p2, 1024 + 24, 2)
        struct.pack_into("<H", p2, 1024 + 0x38, audit.EXT_SUPER_MAGIC)
        (self.root / "emmc-p2.img").write_bytes(p2)

        lines = [hashlib.sha256((self.root / name).read_bytes()).hexdigest() + "  " + name
                 for name, _ in tasks]
        (self.root / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="ascii")

    def test_audit_reports_expected_layout_and_redacts_mac_value(self):
        out = audit.audit_backup(self.root, run_debugfs=False)
        self.assertIn("boot0_ivt_0x400=True", out)
        self.assertIn("U-Boot 2017.03-g8ba4c5bb19-dirty", out)
        self.assertIn("pre_p1_env_0xE0000_crc_ok=True", out)
        self.assertIn("pre_p1_redund_0xE2000_all_zero=True", out)
        self.assertIn("has_ethaddr=True", out)
        self.assertNotIn("00:01:1f:2d:3e:4d", out)
        self.assertIn("p3_bytes=16384 nonzero_chunks=0/1", out)

    def test_corrupted_env_crc_reported_false(self):
        pre = bytearray((self.root / "emmc-reserved-pre-p1.bin").read_bytes())
        pre[0xE0005] ^= 0xFF
        (self.root / "emmc-reserved-pre-p1.bin").write_bytes(pre)
        tasks, _ = audit.verify_seg.layout()
        lines = [hashlib.sha256((self.root / name).read_bytes()).hexdigest() + "  " + name
                 for name, _ in tasks]
        (self.root / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="ascii")
        out = audit.audit_backup(self.root, run_debugfs=False)
        self.assertIn("pre_p1_env_0xE0000_crc_ok=False", out)


if __name__ == "__main__":
    unittest.main()
