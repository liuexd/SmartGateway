#!/usr/bin/env python3
"""Read-only offline audit of a verified G1 eMMC backup directory (U04/U05/boot0)."""
import argparse
import re
import shutil
import struct
import subprocess
import sys
import zlib
from pathlib import Path

import g1_emmc_backup_verify as source
import g1_verify_segments as verify_seg


IVT_MAGIC = b"\xd1\x00\x20\x40"
EXT_SUPER_MAGIC = 0xEF53
ENV_SIZE = 0x2000


def parse_uboot_env_block(block):
    """Parse a 4-byte LE CRC32 + NUL-separated U-Boot environment block."""
    if len(block) < 8:
        return False, 0, {}
    stored_crc = struct.unpack_from("<I", block, 0)[0]
    payload = block[4:]
    calc_crc = zlib.crc32(payload) & 0xFFFFFFFF
    if stored_crc != calc_crc:
        return False, stored_crc, {}
    items = {}
    for raw in payload.split(b"\x00"):
        if not raw:
            break
        text = raw.decode("latin1")
        if "=" in text:
            k, v = text.split("=", 1)
            items[k] = v
    return True, stored_crc, items


def find_uboot_banners(data):
    return [m.decode("ascii", errors="replace")
            for m in re.findall(b"U-Boot 20[0-9]{2}\\.[0-9]{2}[^\x00\r\n]{0,80}", data)]


def scan_zero_prefix_and_nonzero_chunks(path, chunk_size=1024 * 1024):
    total = path.stat().st_size
    first_nz = None
    nz_chunks = 0
    total_chunks = 0
    with path.open("rb") as f:
        off = 0
        while True:
            buf = f.read(chunk_size)
            if not buf:
                break
            total_chunks += 1
            if any(buf):
                nz_chunks += 1
                if first_nz is None:
                    for i, b in enumerate(buf):
                        if b != 0:
                            first_nz = off + i
                            break
            off += len(buf)
    return total, nz_chunks, total_chunks, first_nz


def read_ext_superblock(path, fs_offset):
    with path.open("rb") as f:
        f.seek(fs_offset + 1024)
        sb = f.read(256)
    if len(sb) < 256:
        return None
    magic = struct.unpack_from("<H", sb, 0x38)[0]
    if magic != EXT_SUPER_MAGIC:
        return None
    inodes, blocks = struct.unpack_from("<II", sb, 0)
    free_blocks, free_inodes = struct.unpack_from("<II", sb, 12)
    log_bsize = struct.unpack_from("<I", sb, 24)[0]
    bsize = 1024 << log_bsize
    mtime, wtime = struct.unpack_from("<II", sb, 44)
    return {
        "inodes": inodes,
        "blocks": blocks,
        "free_blocks": free_blocks,
        "free_inodes": free_inodes,
        "block_size": bsize,
        "used_bytes": (blocks - free_blocks) * bsize,
        "fs_bytes": blocks * bsize,
        "mtime": mtime,
        "wtime": wtime,
    }


def run_debugfs_ls(image_path, dir_path):
    debugfs = shutil.which("debugfs") or "/sbin/debugfs"
    if not Path(debugfs).exists():
        return None
    proc = subprocess.run(
        [debugfs, "-R", f"ls -l {dir_path}", str(image_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        check=False,
    )
    if proc.returncode != 0:
        return None
    entries = []
    for line in proc.stdout.splitlines():
        parts = line.split()
        if len(parts) >= 9 and parts[0].isdigit() and parts[-1] not in (".", ".."):
            entries.append(parts[-1])
    return entries


def audit_backup(backup_dir: Path, run_debugfs: bool = True):
    verify_seg.verify(backup_dir)

    pre_path = backup_dir / "emmc-reserved-pre-p1.bin"
    pre_data = pre_path.read_bytes()
    pre_uboot_ivt = pre_data[0x400:0x404] == IVT_MAGIC
    pre_banners = find_uboot_banners(pre_data)
    legacy_crc_ok, _, legacy_env = parse_uboot_env_block(pre_data[0xC0000:0xC0000 + ENV_SIZE])
    active_crc_ok, active_crc, active_env = parse_uboot_env_block(pre_data[0xE0000:0xE0000 + ENV_SIZE])
    redund_zero = not any(pre_data[0xE2000:0xE2000 + ENV_SIZE])

    boot0_data = (backup_dir / "emmc-boot0.img").read_bytes()
    boot1_data = (backup_dir / "emmc-boot1.img").read_bytes()
    boot0_ivt = boot0_data[0x400:0x404] == IVT_MAGIC
    boot0_banners = find_uboot_banners(boot0_data)
    boot1_ivt = boot1_data[0x400:0x404] == IVT_MAGIC
    boot1_banners = find_uboot_banners(boot1_data)

    p1_path = backup_dir / "emmc-p1.img"
    p1_sz, p1_nz_chunks, p1_chunks, p1_first_nz = scan_zero_prefix_and_nonzero_chunks(p1_path)
    p1_sb_0 = read_ext_superblock(p1_path, 0)
    p1_sb_50m = read_ext_superblock(p1_path, 50 * 1024 * 1024)

    p3_path = backup_dir / "emmc-p3.img"
    p3_sz, p3_nz_chunks, p3_chunks, p3_first_nz = scan_zero_prefix_and_nonzero_chunks(p3_path)

    tail_path = backup_dir / "emmc-tail.img"
    tail_sz, tail_nz_chunks, tail_chunks, tail_first_nz = scan_zero_prefix_and_nonzero_chunks(tail_path)
    tail_sb_0 = read_ext_superblock(tail_path, 0)

    p2_path = backup_dir / "emmc-p2.img"
    p2_sb = read_ext_superblock(p2_path, 0)

    p2_tree = {}
    if run_debugfs and p2_sb is not None:
        for d in ("/", "/boot", "/etc/swupdate", "/etc/ssh", "/opt/smartgateway",
                  "/opt/smartgateway/state", "/opt/smartgateway/packages", "/root", "/tmp", "/mnt"):
            entries = run_debugfs_ls(p2_path, d)
            if entries is not None:
                p2_tree[d] = entries

    swu_hits = []
    for d, entries in p2_tree.items():
        for name in entries:
            if name.endswith(".swu") or name == "sw-description":
                swu_hits.append(f"{d.rstrip('/')}/{name}")

    lines = [
        f"AUDIT_TARGET={backup_dir}",
        f"pre_p1_ivt_0x400={pre_uboot_ivt} banners={pre_banners}",
        f"pre_p1_env_0xC0000_crc_ok={legacy_crc_ok} var_count={len(legacy_env)}",
        (
            f"pre_p1_env_0xE0000_crc_ok={active_crc_ok} crc=0x{active_crc:08x} "
            f"var_count={len(active_env)} mmcdev={active_env.get('mmcdev')} "
            f"mmcpart={active_env.get('mmcpart')} mmcroot={active_env.get('mmcroot')!r} "
            f"update={active_env.get('update')} panel={active_env.get('panel')} "
            f"has_ethaddr={'ethaddr' in active_env} has_eth1addr={'eth1addr' in active_env}"
        ),
        f"pre_p1_redund_0xE2000_all_zero={redund_zero}",
        f"boot0_ivt_0x400={boot0_ivt} banners={boot0_banners}",
        f"boot1_ivt_0x400={boot1_ivt} banners={boot1_banners}",
        (
            f"p1_bytes={p1_sz} nonzero_chunks={p1_nz_chunks}/{p1_chunks} "
            f"first_nonzero_offset={p1_first_nz} sb_at_0={p1_sb_0} sb_at_50MiB={p1_sb_50m}"
        ),
        f"p3_bytes={p3_sz} nonzero_chunks={p3_nz_chunks}/{p3_chunks} first_nonzero_offset={p3_first_nz}",
        f"tail_bytes={tail_sz} nonzero_chunks={tail_nz_chunks}/{tail_chunks} first_nonzero_offset={tail_first_nz} sb_at_0={tail_sb_0}",
        f"p2_sb={p2_sb}",
    ]
    if p2_tree:
        lines.append(f"p2_boot_entries={p2_tree.get('/boot', [])}")
        lines.append(f"p2_swupdate_entries={p2_tree.get('/etc/swupdate', [])}")
        lines.append(f"p2_opt_entries={p2_tree.get('/opt/smartgateway', [])}")
        lines.append(f"p2_ssh_host_keys={[x for x in p2_tree.get('/etc/ssh', []) if x.startswith('ssh_host_')]}")
        lines.append(f"p2_swu_or_sw_description_hits={swu_hits}")
    return "\n".join(lines)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("directory", type=Path, help="completed G1 backup directory")
    p.add_argument("--no-debugfs", action="store_true", help="skip debugfs directory listing")
    args = p.parse_args()
    try:
        print(audit_backup(args.directory, run_debugfs=not args.no_debugfs))
    except (verify_seg.VerificationError, source.SafetyError, OSError, ValueError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
