#!/usr/bin/env python3
"""Offline, p2-only factory recovery bundle packager, verifier, and fail-closed restorer.

Strictly scoped to recovering /dev/mmcblk1p2 from an offline verified recovery
bundle (typically under the USB BootROM RAM rescue environment). Leaves
g1_restore_segments.py and production sgw_bootctl apply-bundle p2 protections intact.

Default restore mode is dry-run preflight; actual partition write requires
explicit --confirm-restore-p2.
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import json
import lzma
import os
import shutil
import stat
import sys
from pathlib import Path

TOOL_DIR = Path(__file__).resolve().parent
if str(TOOL_DIR) not in sys.path:
    sys.path.insert(0, str(TOOL_DIR))

import g1_emmc_backup_verify as source
import g1_restore_segments as restore_seg

P2_NAME = "mmcblk1p2"
P2_START_SECTOR = 2052098
P2_SECTOR_COUNT = 3072000
P2_BYTE_SIZE = P2_SECTOR_COUNT * source.SECTOR  # 1,572,864,000 bytes (1500 MiB)
BOARD_ID = "imx6ull-100ask"
EMMC_ID = "mmcblk1"
CHUNK_SIZE = 1024 * 1024


class P2BundleError(Exception):
    pass


def check(ok: bool, msg: str):
    if not ok:
        raise P2BundleError(msg)


def sha256_file(path: Path) -> tuple[str, int]:
    h = hashlib.sha256()
    total = 0
    with path.open("rb") as f:
        while True:
            buf = f.read(CHUNK_SIZE)
            if not buf:
                break
            h.update(buf)
            total += len(buf)
    return h.hexdigest(), total


def open_decompressed_stream(img_path: Path, compression: str):
    if compression == "none":
        return img_path.open("rb")
    if compression == "gzip":
        return gzip.open(img_path, "rb")
    if compression == "xz":
        return lzma.open(img_path, "rb")
    raise P2BundleError(f"unsupported compression format: {compression!r}")


def verify_raw_stream(img_path: Path, compression: str, expected_size: int, expected_sha256: str):
    h = hashlib.sha256()
    total = 0
    with open_decompressed_stream(img_path, compression) as stream:
        while True:
            buf = stream.read(CHUNK_SIZE)
            if not buf:
                break
            h.update(buf)
            total += len(buf)
    check(total == expected_size,
          f"decompressed raw length mismatch for {img_path.name}: {total} != {expected_size}")
    actual_sha = h.hexdigest()
    check(actual_sha == expected_sha256,
          f"decompressed raw SHA-256 mismatch for {img_path.name}: {actual_sha} != {expected_sha256}")


def parse_sha256sums(path: Path) -> dict[str, str]:
    check(path.is_file() and not path.is_symlink(), f"missing or unsafe SHA256SUMS: {path}")
    out: dict[str, str] = {}
    for line in path.read_text(encoding="ascii").splitlines():
        if not line.strip():
            continue
        parts = line.split("  ", 1)
        check(len(parts) == 2 and len(parts[0]) == 64, f"malformed SHA256SUMS line: {line!r}")
        digest, rel = parts
        check(rel not in out, f"duplicate SHA256SUMS entry: {rel}")
        out[rel] = digest
    return out


def verify_bundle(bundle_dir: Path) -> dict:
    check(bundle_dir.is_dir() and not bundle_dir.is_symlink(), f"invalid bundle directory: {bundle_dir}")
    manifest_path = bundle_dir / "recovery-manifest.json"
    sums_path = bundle_dir / "SHA256SUMS"
    check(manifest_path.is_file() and not manifest_path.is_symlink(),
          f"missing recovery-manifest.json in {bundle_dir}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))

    check(manifest.get("schema") == "SGW-P2-RECOVERY-1",
          f"unsupported recovery manifest schema: {manifest.get('schema')!r}")
    check(manifest.get("board") == BOARD_ID,
          f"unexpected board in manifest: {manifest.get('board')!r}")
    check(manifest.get("emmc_device") == EMMC_ID,
          f"unexpected emmc_device in manifest: {manifest.get('emmc_device')!r}")
    check(manifest.get("partition") == P2_NAME,
          f"unexpected partition in manifest: {manifest.get('partition')!r}")
    check(int(manifest.get("start_sector", -1)) == P2_START_SECTOR,
          f"unexpected start_sector: {manifest.get('start_sector')!r}")
    check(int(manifest.get("sector_count", -1)) == P2_SECTOR_COUNT,
          f"unexpected sector_count: {manifest.get('sector_count')!r}")
    raw_size = int(manifest.get("raw_size_bytes", -1))
    check(raw_size > 0 and raw_size == P2_SECTOR_COUNT * source.SECTOR,
          f"unexpected raw_size_bytes: {raw_size}")
    raw_sha = manifest.get("raw_sha256", "")
    check(len(raw_sha) == 64, "invalid raw_sha256 in manifest")

    img_rel = manifest.get("image_file", "")
    check(bool(img_rel) and "/" not in img_rel and ".." not in img_rel,
          f"unsafe image_file in manifest: {img_rel!r}")
    img_path = bundle_dir / img_rel
    check(img_path.is_file() and not img_path.is_symlink(), f"missing recovery image: {img_path}")

    compression = manifest.get("compression", "none")
    check(compression in ("none", "gzip", "xz"), f"unsupported compression: {compression!r}")

    sums = parse_sha256sums(sums_path)
    check(img_rel in sums, f"{img_rel} missing from SHA256SUMS")
    check("recovery-manifest.json" in sums, "recovery-manifest.json missing from SHA256SUMS")
    for rel, expected_digest in sums.items():
        check(not rel.startswith("/") and ".." not in Path(rel).parts,
              f"unsafe path in SHA256SUMS: {rel!r}")
        fp = bundle_dir / rel
        check(fp.is_file() and not fp.is_symlink(), f"missing or symlink file in SHA256SUMS: {fp}")
        actual_digest, _ = sha256_file(fp)
        check(actual_digest == expected_digest,
              f"SHA256SUMS mismatch for {rel}: {actual_digest} != {expected_digest}")

    # Always verify the decompressed raw image SHA-256 and exact byte length.
    verify_raw_stream(img_path, compression, raw_size, raw_sha)
    return manifest


def write_runbook_text(manifest: dict) -> str:
    return f"""# SmartGateway Offline Factory p2 Recovery Runbook

- **Board:** `{manifest['board']}`
- **Target eMMC Partition:** `/dev/{manifest['partition']}` (parent `{manifest['emmc_device']}`, start_sector `{manifest['start_sector']}`, sector_count `{manifest['sector_count']}`, `{manifest['raw_size_bytes']}` bytes)
- **Image File:** `{manifest['image_file']}` (compression: `{manifest['compression']}`)
- **Decompressed Raw SHA-256:** `{manifest['raw_sha256']}`

## Recovery Steps (USB BootROM Rescue Environment)

1. Power off the board, switch boot DIP to USB BootROM (`SW3=OFF, SW4=ON`), connect USB OTG + debug UART.
2. From host, boot the offline USB rescue image in `usb-rescue/`:
   ```bash
   uuu usb-rescue/g1_sdp_ram_boot.uuu
   ```
   (If U-Boot stops at prompt, run `bootz` to enter `rescue-initramfs-v2` RAM shell.)
3. In the RAM rescue shell, verify `/dev/mmcblk1p2` is **unmounted** and read-only (`ro=1`).
4. Verify the offline recovery bundle (dry-run preflight, no writes):
   ```bash
   python3 tools/g1_restore_p2_bundle.py verify .
   python3 tools/g1_restore_p2_bundle.py restore . --target-part /dev/mmcblk1p2
   ```
5. Execute `p2`-only restore (explicit authorization required; writes and verifies full partition SHA-256, leaving `p1`, `p3`, `boot0`, `boot1`, and U-Boot env untouched):
   ```bash
   python3 tools/g1_restore_p2_bundle.py restore . --target-part /dev/mmcblk1p2 --confirm-restore-p2
   ```
6. Re-verify partition digest before switching DIP back to eMMC boot (`SW3=ON, SW4=OFF`):
   ```bash
   sha256sum /dev/mmcblk1p2
   ```
   Expected: `{manifest['raw_sha256']}`
7. Do NOT modify U-Boot env automatically. Power off, restore eMMC boot DIP, and verify via serial console before any separate slot switch.
"""


def cmd_pack(args):
    src_img = Path(args.image).resolve()
    out_dir = Path(args.out_dir).resolve()
    check(src_img.is_file() and not src_img.is_symlink(), f"missing source p2 image: {src_img}")
    raw_sha, raw_size = sha256_file(src_img)
    check(raw_size == P2_BYTE_SIZE,
          f"source p2 image size {raw_size} != expected {P2_BYTE_SIZE} ({P2_SECTOR_COUNT} sectors)")

    if args.expected_sha256:
        check(raw_sha == args.expected_sha256,
              f"source p2 SHA-256 {raw_sha} != expected {args.expected_sha256}")

    check(not out_dir.exists(), f"destination already exists: {out_dir}")
    out_dir.mkdir(parents=True, mode=0o700)

    if args.compress == "none":
        img_name = "emmc-p2.img"
        shutil.copyfile(src_img, out_dir / img_name)
    elif args.compress == "gzip":
        img_name = "emmc-p2.img.gz"
        with src_img.open("rb") as fin, gzip.open(out_dir / img_name, "wb", compresslevel=6) as fout:
            shutil.copyfileobj(fin, fout, length=CHUNK_SIZE)
    else:
        img_name = "emmc-p2.img.xz"
        with src_img.open("rb") as fin, lzma.open(out_dir / img_name, "wb") as fout:
            shutil.copyfileobj(fin, fout, length=CHUNK_SIZE)
    (out_dir / img_name).chmod(0o600)

    manifest = {
        "schema": "SGW-P2-RECOVERY-1",
        "board": BOARD_ID,
        "emmc_device": EMMC_ID,
        "partition": P2_NAME,
        "start_sector": P2_START_SECTOR,
        "sector_count": P2_SECTOR_COUNT,
        "raw_size_bytes": raw_size,
        "raw_sha256": raw_sha,
        "image_file": img_name,
        "compression": args.compress,
        "source_label": args.label,
    }
    manifest_path = out_dir / "recovery-manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    manifest_path.chmod(0o600)

    runbook_path = out_dir / "RECOVERY-RUNBOOK.md"
    runbook_path.write_text(write_runbook_text(manifest), encoding="utf-8")
    runbook_path.chmod(0o600)

    tools_dir = out_dir / "tools"
    tools_dir.mkdir(mode=0o700)
    for tname in (
        "g1_restore_p2_bundle.py",
        "g1_emmc_backup_verify.py",
        "g1_verify_segments.py",
        "g1_restore_segments.py",
    ):
        tpath = TOOL_DIR / tname
        if tpath.is_file():
            shutil.copyfile(tpath, tools_dir / tname)
            (tools_dir / tname).chmod(0o755 if tname.endswith(".py") else 0o644)

    if args.usb_rescue_dir:
        rescue_src = Path(args.usb_rescue_dir).resolve()
        check(rescue_src.is_dir(), f"missing usb_rescue_dir: {rescue_src}")
        rescue_dst = out_dir / "usb-rescue"
        rescue_dst.mkdir(mode=0o700)
        for item in sorted(rescue_src.iterdir()):
            if item.is_file() and not item.is_symlink():
                shutil.copyfile(item, rescue_dst / item.name)
                (rescue_dst / item.name).chmod(0o644)

    # Generate SHA256SUMS covering all files in bundle
    sum_lines = []
    for fp in sorted(out_dir.rglob("*")):
        if fp.is_file() and fp.name != "SHA256SUMS":
            rel = fp.relative_to(out_dir).as_posix()
            d, _ = sha256_file(fp)
            sum_lines.append(f"{d}  {rel}\n")
    sums_path = out_dir / "SHA256SUMS"
    sums_path.write_text("".join(sum_lines), encoding="ascii")
    sums_path.chmod(0o600)

    verify_bundle(out_dir)
    print(f"PACKED_P2_RECOVERY {out_dir} raw_size={raw_size} raw_sha256={raw_sha} compress={args.compress}")


def validate_p2_block_target(target_part: Path, expected_bytes: int,
                             mountinfo_path: Path, sysroot: Path):
    check(not target_part.is_symlink(), f"refusing symlink target: {target_part}")
    target_dev = source.device_info(target_part)

    mounts = source.parse_mountinfo(mountinfo_path.read_text().splitlines())
    mounted_devs, mounted_phys, root_phys = restore_seg.mounted_physical_targets(mounts, sysroot)

    check(target_dev not in mounted_devs, f"refusing mounted target: {target_part}")
    sys_path = source.sys_block(target_dev, sysroot)
    for m_dev in mounted_devs:
        m_sys = sysroot / "dev/block" / f"{m_dev[0]}:{m_dev[1]}"
        if m_sys.exists() and m_sys.resolve() == sys_path:
            raise P2BundleError(f"refusing mounted partition: {target_part}")

    phys = source.physical_devices(target_dev, sysroot)
    check(not (phys & root_phys), f"refusing device backing active '/': {target_part}")
    check((sys_path / "partition").exists(),
          f"p2 restore requires a partition device, not whole disk: {target_part}")

    parent_dir = sys_path.parent
    check(parent_dir.name == EMMC_ID,
          f"wrong parent disk {parent_dir.name!r}; expected {EMMC_ID!r}")
    check(sys_path.name == P2_NAME,
          f"wrong partition {sys_path.name!r}; expected {P2_NAME!r}")

    start = source.read_number(sys_path / "start")
    size = source.read_number(sys_path / "size")
    check((start, size) == (P2_START_SECTOR, P2_SECTOR_COUNT),
          f"p2 geometry mismatch: ({start}, {size}) != ({P2_START_SECTOR}, {P2_SECTOR_COUNT})")

    rfd = os.open(target_part, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
    try:
        st = os.fstat(rfd)
        avail = restore_seg.block_size_bytes(rfd, target_part, st, sysroot)
        check(avail == expected_bytes,
              f"target p2 capacity {avail} != expected {expected_bytes}")
    finally:
        os.close(rfd)
    return target_dev


def cmd_verify(args):
    manifest = verify_bundle(Path(args.bundle_dir).resolve())
    print(f"VERIFIED_P2_BUNDLE {manifest['image_file']} raw_size={manifest['raw_size_bytes']} raw_sha256={manifest['raw_sha256']}")


def cmd_restore(args):
    bundle_dir = Path(args.bundle_dir).resolve()
    target_part = Path(os.path.abspath(args.target_part))
    manifest = verify_bundle(bundle_dir)

    raw_size = int(manifest["raw_size_bytes"])
    raw_sha = manifest["raw_sha256"]
    img_path = bundle_dir / manifest["image_file"]
    compression = manifest["compression"]
    dry_run = not args.confirm_restore_p2

    if not args.allow_regular_file:
        target_dev = validate_p2_block_target(target_part, raw_size, args.mountinfo, args.sysroot)
        toggle_ro = not args.no_toggle_ro
        with restore_seg.BlkRoGuard(target_part, target_dev, enable=toggle_ro, dry_run=dry_run):
            if compression == "none":
                restore_seg.write_and_verify_segment(
                    img_path, target_part, 0, raw_size, raw_sha,
                    False, dry_run, args.sysroot, expected_dev=target_dev,
                )
            else:
                with open_decompressed_stream(img_path, compression) as stream:
                    restore_seg.write_and_verify_segment(
                        None, target_part, 0, raw_size, raw_sha,
                        False, dry_run, args.sysroot, expected_dev=target_dev,
                        src_stream=stream,
                    )
    else:
        check(not target_part.is_symlink(), f"refusing symlink target: {target_part}")
        check(target_part.is_file(), f"regular file target missing: {target_part}")
        check(target_part.stat().st_size == raw_size,
              f"target file size {target_part.stat().st_size} != expected {raw_size}")
        if compression == "none":
            restore_seg.write_and_verify_segment(
                img_path, target_part, 0, raw_size, raw_sha,
                True, dry_run, args.sysroot,
            )
        else:
            with open_decompressed_stream(img_path, compression) as stream:
                restore_seg.write_and_verify_segment(
                    None, target_part, 0, raw_size, raw_sha,
                    True, dry_run, args.sysroot, src_stream=stream,
                )

    status = "PREFLIGHT_OK" if dry_run else "RESTORED_P2"
    print(f"{status} {img_path.name} -> {target_part} ({raw_size} bytes) sha256={raw_sha}")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    sub = p.add_subparsers(dest="cmd", required=True)

    pk = sub.add_parser("pack", help="create offline p2 recovery bundle")
    pk.add_argument("--image", required=True, help="verified emmc-p2.img path")
    pk.add_argument("--out-dir", required=True, help="new output bundle directory")
    pk.add_argument("--compress", choices=("none", "gzip", "xz"), default="none")
    pk.add_argument("--expected-sha256", default=None)
    pk.add_argument("--label", default="factory-p2")
    pk.add_argument("--usb-rescue-dir", default=None, help="optional USB BootROM boot assets dir")

    vf = sub.add_parser("verify", help="verify recovery bundle SHA256SUMS and decompressed raw image")
    vf.add_argument("bundle_dir")

    rs = sub.add_parser("restore", help="preflight (default) or write p2-only recovery image")
    rs.add_argument("bundle_dir")
    rs.add_argument("--target-part", required=True, help="target /dev/mmcblk1p2 partition")
    rs.add_argument("--confirm-restore-p2", action="store_true",
                    help="required explicit authorization to actually write p2")
    rs.add_argument("--no-toggle-ro", action="store_true",
                    help="do not toggle BLKROSET on block target")
    rs.add_argument("--allow-regular-file", action="store_true",
                    help="isolated regular-file rehearsal only")
    rs.add_argument("--mountinfo", type=Path, default=Path("/proc/self/mountinfo"))
    rs.add_argument("--sysroot", type=Path, default=Path("/sys"))

    args = p.parse_args()
    try:
        if args.cmd == "pack":
            cmd_pack(args)
        elif args.cmd == "verify":
            cmd_verify(args)
        else:
            cmd_restore(args)
    except (P2BundleError, restore_seg.RestoreError, source.SafetyError,
            OSError, ValueError, json.JSONDecodeError) as exc:
        print(f"ERROR: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
