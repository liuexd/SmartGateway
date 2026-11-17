#!/usr/bin/env python3
"""Host-only isolated tests for the proposed SSH backup transport."""
import hashlib
import io
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bsp/tools'))
import g1_receive_segments as receiver
import g1_stream_segment as sender


class TransportTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='g1-stream-test-')
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.dfd = os.open(self.root, os.O_RDONLY | os.O_DIRECTORY)
        self.addCleanup(os.close, self.dfd)

    def test_complete_ssh_stream_hash_and_readback(self):
        data = b'backup-data-1234'
        digest = hashlib.sha256(data).hexdigest()
        code = ('import sys; sys.stdout.buffer.write(%r); sys.stdout.flush(); '
                'sys.stderr.write("G1_SHA256=%s\\n")') % (data, digest)
        result = receiver.receive_one([sys.executable, '-c', code], self.dfd, 'part', len(data), 5)
        self.assertEqual(result, digest)
        self.assertEqual((self.root / 'part').read_bytes(), data)

    def test_short_excess_and_wrong_digest_rejected(self):
        for name, data, size, digest in [('short', b'123', 4, '0'*64),
                                         ('extra', b'12345', 4, '0'*64),
                                         ('digest', b'1234', 4, '0'*64)]:
            with self.subTest(name=name):
                code = ('import sys; sys.stdout.buffer.write(%r); sys.stdout.flush(); '
                        'sys.stderr.write("G1_SHA256=%s\\n")') % (data, digest)
                with self.assertRaises(receiver.ReceiveError):
                    receiver.receive_one([sys.executable, '-c', code], self.dfd, name, size, 5)

    def test_nonzero_and_timeout_rejected(self):
        code = 'import sys; sys.stdout.buffer.write(b"1234"); sys.stdout.flush(); sys.exit(2)'
        with self.assertRaises(receiver.ReceiveError):
            receiver.receive_one([sys.executable, '-c', code], self.dfd, 'failed', 4, 5)
        with self.assertRaisesRegex(receiver.ReceiveError, 'stalled'):
            receiver.receive_one([sys.executable, '-c', 'import time; time.sleep(5)'], self.dfd, 'stalled', 4, 0.1)

    def test_existing_output_and_symlink_rejected(self):
        (self.root / 'existing').write_text('preserve')
        (self.root / 'link').symlink_to('existing')
        for name in ('existing', 'link'):
            with self.assertRaises(OSError):
                receiver.receive_one([sys.executable, '-c', 'pass'], self.dfd, name, 0, 1)
        self.assertEqual((self.root / 'existing').read_text(), 'preserve')

    def test_incomplete_transfer_never_creates_manifest(self):
        identity = self.root / 'identity'
        known = self.root / 'known_hosts'
        identity.write_text('private-test-key')
        identity.chmod(0o600)
        known.write_text('known-test-host')
        known.chmod(0o600)
        dest = self.root / 'backup'
        with patch.object(receiver.archive, 'layout', return_value=([('emmc-p1.img', 4)], 1)), \
             patch.object(receiver, 'receive_one', side_effect=receiver.ReceiveError('source unavailable')) as read:
            with self.assertRaises(receiver.ReceiveError):
                receiver.receive(dest, '127.0.0.1', 2222, identity, known)
        self.assertTrue(dest.is_dir())
        self.assertFalse((dest / 'SHA256SUMS').exists())
        command = read.call_args.args[0]
        self.assertIn('StrictHostKeyChecking=yes', command)
        self.assertIn('IdentitiesOnly=yes', command)
        self.assertIn('root@127.0.0.1', command)

    def test_rejects_untrusted_key_and_existing_destination(self):
        identity = self.root / 'identity'
        known = self.root / 'known_hosts'
        identity.write_text('private-test-key')
        identity.chmod(0o644)
        known.write_text('known-test-host')
        known.chmod(0o600)
        with self.assertRaisesRegex(receiver.ReceiveError, 'permissions'):
            receiver.receive(self.root / 'unused', '127.0.0.1', 2222, identity, known)
        identity.chmod(0o600)
        with self.assertRaisesRegex(receiver.ReceiveError, 'destination already exists'):
            receiver.receive(self.root, '127.0.0.1', 2222, identity, known)

    def test_sender_aborts_before_open_on_writable_source(self):
        with patch.object(sender.g1, 'validate_devices', return_value=(Path('/x'), (179, 0), {})), \
             patch.object(sender.g1, 'parse_mountinfo', return_value=[]), \
             patch.object(Path, 'read_text', return_value='mountinfo'), \
             patch.object(sender.g1, 'assert_no_rw_emmc', side_effect=sender.g1.SafetyError('mounted rw')), \
             patch.object(sender.os, 'open') as opened:
            with self.assertRaises(sender.g1.SafetyError):
                sender.stream('emmc-p2.img', io.BytesIO())
            opened.assert_not_called()

    def test_sender_streams_exact_range_after_all_guards(self):
        source = self.root / 'source'
        source.write_bytes(b'ABCDEFGHIJ')
        devices = {'mmcblk1p1': ((0, 0), 2), 'mmcblk1p2': ((0, 0), 4),
                   'mmcblk1p3': ((0, 0), 2), 'mmcblk1boot0': ((0, 0), 2),
                   'mmcblk1boot1': ((0, 0), 2)}
        geometry = {'mmcblk1p1': (1, 1), 'mmcblk1p2': (2, 2), 'mmcblk1p3': (4, 1)}
        real_open = os.open
        def fake_open(path, flags, *args, **kwargs):
            return real_open(source, flags)
        output = io.BytesIO()
        with patch.object(sender.g1, 'validate_devices', return_value=(Path('/x'), (0, 0), devices)), \
             patch.object(sender.g1, 'parse_mountinfo', return_value=[]), \
             patch.object(Path, 'read_text', return_value='mountinfo'), \
             patch.object(sender.g1, 'assert_no_rw_emmc'), \
             patch.object(sender.g1, 'assert_readonly'), \
             patch.object(sender.g1, 'GEOMETRY', geometry), \
             patch.object(sender.g1, 'PRE_SECTORS', 1), \
             patch.object(sender.g1, 'USER_SECTORS', 6), \
             patch.object(sender.g1, 'SECTOR', 1), \
             patch.object(sender.stat, 'S_ISBLK', return_value=True), \
             patch.object(sender.os, 'open', side_effect=fake_open):
            digest = sender.stream('emmc-p2.img', output)
        self.assertEqual(output.getvalue(), b'ABCD')
        self.assertEqual(digest, hashlib.sha256(b'ABCD').hexdigest())

if __name__ == '__main__':
    unittest.main()
