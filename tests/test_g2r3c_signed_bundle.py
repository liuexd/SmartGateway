#!/usr/bin/env python3
"""Only isolated files and test keys; no real firmware/device/env is modified."""
import fcntl
import hashlib
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CLI = ROOT / "bsp/apps/boot_ab/r3c_bundle_cli"


class SignedBundleTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix=".sgw-r3c-sign-", dir=Path.home())
        self.addCleanup(self.tmp.cleanup)
        base = Path(self.tmp.name)
        self.bundle = base / "bundle"
        self.tx = base / "tx"
        self.bundle.mkdir(mode=0o700)
        self.tx.mkdir(mode=0o700)
        self.private = base / "test-private.pem"
        self.public = base / "test-public.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                        "rsa_keygen_bits:2048", "-out", str(self.private)],
                       check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(self.private), "-pubout",
                        "-out", str(self.public)], check=True, capture_output=True)
        self.public.chmod(0o600)
        image = b"host-test-image-only\x00" * 64
        (self.bundle / "rootfs.img").write_bytes(image)
        manifest = ("SGW-SYSTEM-1\nboard=imx6ull-100ask\nimage=rootfs.img\n"
                    f"size={len(image)}\nsha256={hashlib.sha256(image).hexdigest()}\n")
        (self.bundle / "manifest").write_text(manifest)
        subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(self.private),
                        "-out", str(self.bundle / "manifest.sig"),
                        str(self.bundle / "manifest")], check=True, capture_output=True)
        for p in self.bundle.iterdir():
            p.chmod(0o600)

    def call(self, target="1", last="2", tx=None):
        return subprocess.run([str(CLI), str(self.bundle), str(tx or self.tx), str(self.public),
                               target, last], capture_output=True, text=True, timeout=10)

    def records(self):
        return list(self.tx.glob("txn-*"))

    def test_signed_package_records_two_distinct_random_ids(self):
        first = self.call()
        self.assertEqual(first.returncode, 0, first.stdout + first.stderr)
        self.assertNotEqual(self.call().returncode, 0)  # unfinished transaction
        tx2 = self.tx.parent / "tx2"
        tx2.mkdir(mode=0o700)
        second = self.call(tx=tx2)
        self.assertEqual(second.returncode, 0, second.stdout + second.stderr)
        a, b = first.stdout.strip(), second.stdout.strip()
        self.assertRegex(a, r"^[0-9a-f]{64}$")
        self.assertNotEqual(a, b)
        record = (self.tx / ("txn-" + a)).read_text()
        self.assertIn("state=VERIFIED\n", record)
        self.assertIn("target=1\nlast_good=2\n", record)
        self.assertIn("image=" + hashlib.sha256((self.bundle / "rootfs.img").read_bytes()).hexdigest(), record)
        self.assertEqual(len(self.records()), 1)
        self.assertTrue((tx2 / ("txn-" + b)).exists())

    def test_tamper_image_manifest_and_signature_rejected(self):
        image = self.bundle / "rootfs.img"
        original = image.read_bytes()
        image.write_bytes(original + b"tamper")
        self.assertNotEqual(self.call().returncode, 0)
        image.write_bytes(original)
        manifest = self.bundle / "manifest"
        original_manifest = manifest.read_bytes()
        manifest.write_bytes(original_manifest + b"unexpected")
        self.assertNotEqual(self.call().returncode, 0)
        manifest.write_bytes(original_manifest)
        sig = self.bundle / "manifest.sig"
        original_sig = sig.read_bytes()
        sig.write_bytes(bytes([original_sig[0] ^ 1]) + original_sig[1:])
        self.assertNotEqual(self.call().returncode, 0)
        self.assertEqual(self.records(), [])

    def test_wrong_slots_untrusted_key_and_unsafe_permissions_rejected(self):
        self.assertNotEqual(self.call("1", "1").returncode, 0)
        self.public.chmod(0o666)
        self.assertNotEqual(self.call().returncode, 0)
        self.public.chmod(0o600)
        (self.bundle / "rootfs.img").chmod(0o666)
        self.assertNotEqual(self.call().returncode, 0)
        (self.bundle / "rootfs.img").chmod(0o600)
        wrong_private = self.tx.parent / "wrong-private.pem"
        subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt",
                        "rsa_keygen_bits:2048", "-out", str(wrong_private)],
                       check=True, capture_output=True)
        subprocess.run(["openssl", "pkey", "-in", str(wrong_private), "-pubout",
                        "-out", str(self.public)], check=True, capture_output=True)
        self.public.chmod(0o600)
        self.assertNotEqual(self.call().returncode, 0)
        self.assertEqual(self.records(), [])

    def test_lock_contention_and_readonly_destination_rejected(self):
        path = self.tx / ".sgw-transaction.lock"
        path.touch(mode=0o600)
        with path.open("r+") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.assertNotEqual(self.call().returncode, 0)
        path.unlink()
        self.tx.chmod(0o500)
        try:
            self.assertNotEqual(self.call().returncode, 0)
        finally:
            self.tx.chmod(0o700)
        self.assertEqual(self.records(), [])

    def test_persistence_failure_never_reports_success(self):
        fail_lib = self.tx.parent / "fsync-fail.so"
        subprocess.run(["gcc", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC",
                        str(ROOT / "tests/r3c_fsync_fail.c"), "-o", str(fail_lib)],
                       check=True, capture_output=True)
        env = os.environ.copy()
        env["LD_PRELOAD"] = str(fail_lib)
        result = subprocess.run([str(CLI), str(self.bundle), str(self.tx),
                                 str(self.public), "1", "2"],
                                env=env, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(result.stdout.strip(), "rejected")
        self.assertEqual(self.records(), [])


if __name__ == "__main__":
    unittest.main(verbosity=2)
