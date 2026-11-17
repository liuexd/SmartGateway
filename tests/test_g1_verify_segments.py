#!/usr/bin/env python3
"""G1 archive verification tests; fixtures use regular files only."""
import contextlib
import hashlib
import io
import struct
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bsp/tools'))
import g1_verify_segments as verify


class BackupSegmentsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='g1-verify-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        geometry = {'mmcblk1p1': (1, 1), 'mmcblk1p2': (2, 2), 'mmcblk1p3': (4, 1)}
        self.patches = [patch.object(verify.source, 'GEOMETRY', geometry),
                        patch.object(verify.source, 'PRE_SECTORS', 1),
                        patch.object(verify.source, 'USER_SECTORS', 6),
                        patch.object(verify.source, 'BOOT_SECTORS', 1)]
        for p in self.patches:
            p.start()
            self.addCleanup(p.stop)
        self.tasks, _ = verify.layout()
        for name, size in self.tasks:
            (self.root / name).write_bytes(bytes([len(name)]) * size)
        header = bytearray((self.root / self.tasks[0][0]).read_bytes())
        header[446:510] = bytes(64)
        for i, ((_, (start, size)), typ) in enumerate(zip(geometry.items(), (0x83, 0x83, 0x0c))):
            header[446+16*i+4] = typ
            struct.pack_into('<II', header, 446+16*i+8, start, size)
        header[510:512] = b'\x55\xaa'
        (self.root / self.tasks[0][0]).write_bytes(header)
        self.write_manifest()

    def write_manifest(self):
        lines = [hashlib.sha256((self.root / name).read_bytes()).hexdigest() + '  ' + name
                 for name, _ in self.tasks]
        (self.root / 'SHA256SUMS').write_text('\n'.join(lines) + '\n', encoding='ascii')

    def test_complete_archive_and_ordered_digest(self):
        stream = io.StringIO()
        with contextlib.redirect_stdout(stream):
            verify.verify(self.root)
        expected = hashlib.sha256(b''.join((self.root / n).read_bytes() for n, _ in self.tasks[:5])).hexdigest()
        self.assertIn(expected, stream.getvalue())
        self.assertIn('restoration remain unverified', stream.getvalue())

    def test_tamper_and_missing_segments_rejected(self):
        p = self.root / 'emmc-p2.img'
        p.write_bytes(b'X' * p.stat().st_size)
        with self.assertRaisesRegex(verify.VerificationError, 'digest mismatch'):
            verify.verify(self.root)
        p.unlink()
        with self.assertRaises(FileNotFoundError):
            verify.verify(self.root)

    def test_wrong_partition_table_rejected_even_if_hash_updated(self):
        p = self.root / 'emmc-reserved-pre-p1.bin'
        data = bytearray(p.read_bytes())
        struct.pack_into('<I', data, 446+8, 7)
        p.write_bytes(data)
        self.write_manifest()
        with self.assertRaisesRegex(verify.VerificationError, 'MBR partition'):
            verify.verify(self.root)

    def test_duplicate_manifest_and_symlink_rejected(self):
        manifest = self.root / 'SHA256SUMS'
        original = manifest.read_text()
        manifest.write_text(original + original.splitlines()[0] + '\n')
        with self.assertRaisesRegex(verify.VerificationError, 'duplicate'):
            verify.verify(self.root)
        manifest.write_text(original)
        p = self.root / 'emmc-p3.img'
        data = p.read_bytes()
        p.unlink()
        (self.root / 'other').write_bytes(data)
        p.symlink_to('other')
        with self.assertRaises(OSError):
            verify.verify(self.root)

if __name__ == '__main__':
    unittest.main()
