#!/usr/bin/env python3
"""Signed manifest verification against the rebuilt G3 ext4, entirely offline."""
import hashlib
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
IMAGE = ROOT / "build/buildroot-g3/images/rootfs.ext4"
ARCHIVE = ROOT / "build/buildroot-g3/images/rootfs.tar"
REQUIRED = (
    "/sbin/init", "/etc/inittab", "/etc/fstab", "/etc/fw_env.config", "/sbin/fw_printenv",
    "/usr/bin/sgw_bootctl", "/usr/bin/sgw_watchdog_guard", "/usr/bin/service_supervisor",
    "/usr/bin/smart_gateway", "/usr/bin/ota_agent", "/etc/init.d/S15sgw_ab_guard",
    "/etc/init.d/S99smartgateway", "/boot/zImage", "/boot/100ask_imx6ull-14x14.dtb",
    "/lib/libc.so.6",
)

class G3BundleTest(unittest.TestCase):
    def test_signed_manifest_matches_real_g3_image(self):
        self.assertTrue(IMAGE.is_file(), "build isolated G3 rootfs first")
        cli = ROOT / "bsp/apps/boot_ab/r3c_bundle_cli"
        self.assertTrue(cli.is_file())
        with tempfile.TemporaryDirectory(prefix=".sgw-g3-sign-", dir=Path.home()) as temp:
            base = Path(temp)
            bundle, txn = base / "bundle", base / "transactions"
            bundle.mkdir(mode=0o700)
            txn.mkdir(mode=0o700)
            shutil.copyfile(IMAGE, bundle / "rootfs.img")
            manifest = []
            with tarfile.open(ARCHIVE) as tar:
                members = {m.name.lstrip("./"): m for m in tar.getmembers()}
                for name in REQUIRED:
                    member = members[name.lstrip("/")]
                    data = tar.extractfile(member)
                    self.assertIsNotNone(data, name)
                    manifest.append(name + " " + hashlib.sha256(data.read()).hexdigest())
            root_manifest = ("\n".join(manifest) + "\n").encode()
            (bundle / "rootfs.manifest").write_bytes(root_manifest)
            image = bundle / "rootfs.img"
            with image.open("rb") as f:
                digest = hashlib.file_digest(f, "sha256").hexdigest()
            signed = ("SGW-SYSTEM-2\nboard=imx6ull-100ask\nimage=rootfs.img\n"
                      f"size={image.stat().st_size}\nsha256={digest}\n"
                      f"rootfs_manifest_sha256={hashlib.sha256(root_manifest).hexdigest()}\n")
            (bundle / "manifest").write_text(signed)
            priv, pub = base / "private.pem", base / "public.pem"
            subprocess.run(["openssl", "genpkey", "-algorithm", "RSA", "-pkeyopt", "rsa_keygen_bits:2048",
                            "-out", str(priv)], check=True, capture_output=True)
            subprocess.run(["openssl", "pkey", "-in", str(priv), "-pubout", "-out", str(pub)],
                           check=True, capture_output=True)
            subprocess.run(["openssl", "dgst", "-sha256", "-sign", str(priv), "-out",
                            str(bundle / "manifest.sig"), str(bundle / "manifest")], check=True)
            for f in bundle.iterdir(): f.chmod(0o600)
            pub.chmod(0o600)
            result = subprocess.run([str(cli), str(bundle), str(txn), str(pub), "1", "2"],
                                    capture_output=True, text=True, timeout=90)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

if __name__ == "__main__":
    unittest.main()
