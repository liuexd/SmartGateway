#!/usr/bin/env python3
"""Fail-closed G1 segmented restore and readback verification tool.

Always verifies the backup archive before opening any target for writing,
blocks writes to mounted targets or protected p2/active-root devices, and
reads back every written byte range to confirm SHA-256 integrity.
"""
import argparse
import array
import fcntl
import hashlib
import os
import stat
import sys
from pathlib import Path

import g1_emmc_backup_verify as source
import g1_verify_segments as verify_seg

BLKGETSIZE64 = 0x80081272
CHUNK_SIZE = 1024 * 1024
PROTECTED_P2_GEOM = source.GEOMETRY["mmcblk1p2"]


class RestoreError(Exception):
    pass


def check(ok, message):
    if not ok:
        raise RestoreError(message)


def block_size_bytes(fd, path, st, sysroot):
    if stat.S_ISREG(st.st_mode):
        return st.st_size
    check(stat.S_ISBLK(st.st_mode), f"target is neither block device nor regular file: {path}")
    dev = (os.major(st.st_rdev), os.minor(st.st_rdev))
    sys_path = source.sys_block(dev, sysroot)
    sectors = source.read_number(sys_path / "size")
    buf = array.array("Q", [0])
    try:
        fcntl.ioctl(fd, BLKGETSIZE64, buf, True)
        if buf[0] > 0:
            return buf[0]
    except OSError:
        pass
    return sectors * source.SECTOR


def mounted_physical_targets(mounts, sysroot):
    mounted_devs = set()
    mounted_phys = set()
    root_phys = set()
    for dev, opts, fs, mountpoint in mounts:
        mounted_devs.add(dev)
        if fs == "squashfs" and "ro" in opts and "rw" not in opts:
            continue
        if (sysroot / "dev/block" / f"{dev[0]}:{dev[1]}").exists():
            phys = source.physical_devices(dev, sysroot)
            mounted_phys.update(phys)
            if mountpoint == "/":
                root_phys.update(phys)
    return mounted_devs, mounted_phys, root_phys


def is_protected_p2_or_active(dev, sysroot, mounted_devs, mounted_phys, root_phys):
    if dev in mounted_devs:
        return True, "target device is currently mounted"
    sys_path = source.sys_block(dev, sysroot)
    phys = source.physical_devices(dev, sysroot)
    if phys & root_phys:
        # Block writing to any partition or whole-disk holding active /
        if not (sys_path / "partition").exists():
            return True, "target whole disk backs active /"
    # Check if this partition itself backs active /
    for m_dev in mounted_devs:
        m_sys = sysroot / "dev/block" / f"{m_dev[0]}:{m_dev[1]}"
        if m_sys.exists() and m_sys.resolve() == sys_path:
            return True, "target partition is currently mounted"
    if (sys_path / "partition").exists():
        start = source.read_number(sys_path / "start")
        size = source.read_number(sys_path / "size")
        if (start, size) == PROTECTED_P2_GEOM or sys_path.name == "mmcblk1p2":
            return True, "target is protected p2 (mmcblk1p2)"
    if sys_path.name == "mmcblk1" or (phys & mounted_phys and not (sys_path / "partition").exists()):
        return True, "target whole disk has active mounted partitions"
    return False, ""


class ForceRoGuard:
    """Temporarily clear sysfs force_ro for eMMC boot hardware partitions."""

    def __init__(self, sys_path: Path, dry_run: bool):
        self.force_ro = sys_path / "force_ro"
        self.prev = None
        self.dry_run = dry_run

    def __enter__(self):
        if not self.dry_run and self.force_ro.exists():
            val = self.force_ro.read_text().strip()
            if val == "1":
                self.force_ro.write_text("0\n")
                self.prev = "1\n"
        return self

    def __exit__(self, exc_type, exc, tb):
        if self.prev is not None and self.force_ro.exists():
            try:
                self.force_ro.write_text(self.prev)
            except OSError:
                pass


def write_and_verify_segment(src_path: Path, dst_path: Path, dst_offset: int,
                             expected_len: int, expected_sha256: str,
                             allow_regular_file: bool, dry_run: bool,
                             sysroot: Path, expected_dev=None):
    check(not dst_path.is_symlink(), f"refusing symlink target: {dst_path}")
    src_dfd = os.open(src_path.parent, os.O_RDONLY | getattr(os, "O_DIRECTORY", 0))
    try:
        src_fd, src_st = verify_seg.open_regular(src_dfd, src_path.name)
    finally:
        os.close(src_dfd)
    try:
        check(src_st.st_size == expected_len, f"source length mismatch for {src_path.name}")
    finally:
        os.close(src_fd)

    if dry_run:
        if dst_path.exists():
            rfd = os.open(dst_path, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0))
            try:
                st = os.fstat(rfd)
                if not allow_regular_file:
                    check(stat.S_ISBLK(st.st_mode), f"target is not a block device: {dst_path}")
                    check((os.major(st.st_rdev), os.minor(st.st_rdev)) == expected_dev, "target device changed")
                avail = block_size_bytes(rfd, dst_path, st, sysroot)
                check(avail >= dst_offset + expected_len, f"target too small: {avail} < {dst_offset + expected_len}")
            finally:
                os.close(rfd)
        return

    flags = os.O_RDWR | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0)
    if allow_regular_file and not dst_path.exists():
        flags |= os.O_CREAT
    dfd = os.open(dst_path, flags, 0o600)
    try:
        st = os.fstat(dfd)
        if allow_regular_file:
            check(stat.S_ISREG(st.st_mode), f"expected regular file target: {dst_path}")
        else:
            check(stat.S_ISBLK(st.st_mode), f"target is not a block device: {dst_path}")
            check((os.major(st.st_rdev), os.minor(st.st_rdev)) == expected_dev, "target device identity changed")
            avail = block_size_bytes(dfd, dst_path, st, sysroot)
            check(avail >= dst_offset + expected_len, f"target too small: {avail} < {dst_offset + expected_len}")

        os.lseek(dfd, dst_offset, os.SEEK_SET)
        digest = hashlib.sha256()
        with src_path.open("rb") as sf:
            remaining = expected_len
            while remaining:
                chunk = sf.read(min(CHUNK_SIZE, remaining))
                check(bool(chunk), f"short read from {src_path}")
                digest.update(chunk)
                view = memoryview(chunk)
                while view:
                    n = os.write(dfd, view)
                    check(n > 0, f"short write to {dst_path}")
                    view = view[n:]
                remaining -= len(chunk)
        check(digest.hexdigest() == expected_sha256, f"source digest changed while writing {src_path.name}")
        os.fsync(dfd)
    finally:
        os.close(dfd)

    # Re-open read-only and verify readback digest
    rfd = os.open(dst_path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0) | getattr(os, "O_NOFOLLOW", 0))
    try:
        os.lseek(rfd, dst_offset, os.SEEK_SET)
        rb_digest = hashlib.sha256()
        remaining = expected_len
        while remaining:
            chunk = os.read(rfd, min(CHUNK_SIZE, remaining))
            check(bool(chunk), f"short readback from {dst_path}")
            rb_digest.update(chunk)
            remaining -= len(chunk)
        check(rb_digest.hexdigest() == expected_sha256,
              f"readback SHA-256 mismatch on {dst_path} @ {dst_offset}")
    finally:
        os.close(rfd)


def parse_sha256sums(backup_dir: Path):
    sums = {}
    for line in (backup_dir / "SHA256SUMS").read_text(encoding="ascii").splitlines():
        digest, name = line.split("  ", 1)
        sums[name] = digest
    return sums


def restore_full_disk(backup_dir: Path, target_disk: Path, boot0: Path | None,
                      boot1: Path | None, allow_regular_file: bool, dry_run: bool,
                      confirm_overwrite_all: bool, mountinfo_path: Path, sysroot: Path):
    check(confirm_overwrite_all, "full-disk restore requires --confirm-overwrite-all")
    verify_seg.verify(backup_dir)
    sums = parse_sha256sums(backup_dir)
    tasks, user_count = verify_seg.layout()

    if not allow_regular_file:
        mounts = source.parse_mountinfo(mountinfo_path.read_text().splitlines())
        mounted_devs, mounted_phys, root_phys = mounted_physical_targets(mounts, sysroot)
    else:
        mounted_devs, mounted_phys, root_phys = set(), set(), set()

    disk_dev = None
    if not allow_regular_file:
        check(not target_disk.is_symlink(), f"refusing symlink target: {target_disk}")
        disk_dev = source.device_info(target_disk)
        disk_phys = source.physical_devices(disk_dev, sysroot)
        check(not (disk_phys & mounted_phys) and not (disk_phys & root_phys),
              f"refusing full-disk restore onto disk with mounted partitions: {target_disk}")
        st = os.stat(target_disk)
        fd = os.open(target_disk, os.O_RDONLY)
        try:
            sz = block_size_bytes(fd, target_disk, st, sysroot)
        finally:
            os.close(fd)
        check(sz == source.USER_SECTORS * source.SECTOR,
              f"target disk size {sz} != expected {source.USER_SECTORS * source.SECTOR}")

    offset = 0
    for name, length in tasks[:user_count]:
        write_and_verify_segment(
            backup_dir / name, target_disk, offset, length, sums[name],
            allow_regular_file, dry_run, sysroot, expected_dev=disk_dev,
        )
        print(f"{'DRY-RUN' if dry_run else 'RESTORED'} {name} -> {target_disk} @ {offset} ({length} bytes)")
        offset += length

    for boot_name, boot_target in (("emmc-boot0.img", boot0), ("emmc-boot1.img", boot1)):
        if boot_target is None:
            continue
        length = source.BOOT_SECTORS * source.SECTOR
        if allow_regular_file:
            write_and_verify_segment(
                backup_dir / boot_name, boot_target, 0, length, sums[boot_name],
                True, dry_run, sysroot,
            )
        else:
            check(not boot_target.is_symlink(), f"refusing symlink target: {boot_target}")
            bdev = source.device_info(boot_target)
            bsys = source.sys_block(bdev, sysroot)
            check(bdev not in mounted_devs, f"boot device mounted: {boot_target}")
            with ForceRoGuard(bsys, dry_run):
                write_and_verify_segment(
                    backup_dir / boot_name, boot_target, 0, length, sums[boot_name],
                    False, dry_run, sysroot, expected_dev=bdev,
                )
        print(f"{'DRY-RUN' if dry_run else 'RESTORED'} {boot_name} -> {boot_target} ({length} bytes)")


def restore_single_segment(backup_dir: Path, segment_name: str, target_part: Path,
                           custom_image: Path | None, custom_sha256: str | None,
                           allow_regular_file: bool, dry_run: bool,
                           mountinfo_path: Path, sysroot: Path):
    verify_seg.verify(backup_dir)
    sums = parse_sha256sums(backup_dir)
    tasks = dict(verify_seg.layout()[0])
    check(segment_name in tasks, f"unknown segment: {segment_name}")
    check(segment_name != "emmc-p2.img", "single-segment mode refuses overwriting protected emmc-p2.img")

    if custom_image is not None:
        check(custom_sha256 is not None and len(custom_sha256) == 64,
              "--image requires 64-hex --sha256")
        src_path = custom_image
        length = custom_image.stat().st_size
        check(length <= tasks[segment_name],
              f"custom image {length} exceeds segment capacity {tasks[segment_name]}")
        expected_sha = custom_sha256
    else:
        src_path = backup_dir / segment_name
        length = tasks[segment_name]
        expected_sha = sums[segment_name]

    if not allow_regular_file:
        mounts = source.parse_mountinfo(mountinfo_path.read_text().splitlines())
        mounted_devs, mounted_phys, root_phys = mounted_physical_targets(mounts, sysroot)
    else:
        mounted_devs, mounted_phys, root_phys = set(), set(), set()

    target_dev = None
    if not allow_regular_file:
        check(not target_part.is_symlink(), f"refusing symlink target: {target_part}")
        target_dev = source.device_info(target_part)
        blocked, reason = is_protected_p2_or_active(
            target_dev, sysroot, mounted_devs, mounted_phys, root_phys
        )
        check(not blocked, reason)
        sys_path = source.sys_block(target_dev, sysroot)
        check((sys_path / "partition").exists(),
              f"single-segment mode requires a partition device, not whole disk: {target_part}")
        write_and_verify_segment(
            src_path, target_part, 0, length, expected_sha,
            False, dry_run, sysroot, expected_dev=target_dev,
        )
    else:
        write_and_verify_segment(
            src_path, target_part, 0, length, expected_sha,
            True, dry_run, sysroot,
        )
    print(f"{'DRY-RUN' if dry_run else 'RESTORED'} {src_path.name} -> {target_part} ({length} bytes)")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("backup_dir", type=Path, help="verified G1 backup directory")
    p.add_argument("--mode", choices=("full-disk", "single-segment"), required=True)
    p.add_argument("--target-disk", type=Path, help="whole disk device/file for full-disk mode")
    p.add_argument("--boot0", type=Path, help="boot0 device/file for full-disk mode")
    p.add_argument("--boot1", type=Path, help="boot1 device/file for full-disk mode")
    p.add_argument("--confirm-overwrite-all", action="store_true",
                   help="explicit confirmation for full-disk restore")
    p.add_argument("--segment", help="segment name for single-segment mode (e.g. emmc-p1.img)")
    p.add_argument("--target-part", type=Path, help="target partition device/file for single-segment mode")
    p.add_argument("--image", type=Path, help="optional verified custom image (e.g. resized ext4) <= segment size")
    p.add_argument("--sha256", help="expected SHA-256 for --image")
    p.add_argument("--allow-regular-file", action="store_true",
                   help="allow writing to a regular non-symlink image file (for isolated testing)")
    p.add_argument("--dry-run", action="store_true", help="run all verification and safety guards without writing")
    p.add_argument("--mountinfo", type=Path, default=Path("/proc/self/mountinfo"))
    p.add_argument("--sysroot", type=Path, default=Path("/sys"))
    args = p.parse_args()

    try:
        if args.mode == "full-disk":
            check(args.target_disk is not None, "--target-disk is required for full-disk mode")
            restore_full_disk(
                args.backup_dir, args.target_disk, args.boot0, args.boot1,
                args.allow_regular_file, args.dry_run, args.confirm_overwrite_all,
                args.mountinfo, args.sysroot,
            )
        else:
            check(args.segment is not None and args.target_part is not None,
                  "--segment and --target-part are required for single-segment mode")
            restore_single_segment(
                args.backup_dir, args.segment, args.target_part,
                args.image, args.sha256, args.allow_regular_file,
                args.dry_run, args.mountinfo, args.sysroot,
            )
    except (RestoreError, verify_seg.VerificationError, source.SafetyError, OSError, ValueError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
