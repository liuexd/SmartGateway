#!/usr/bin/env python3
"""G1 offline backup. Unknown storage topology is an error, not a safe device.

Requires Python 3, Linux mountinfo/sysfs and the audited mmcblk1 geometry.
No test overrides or caller-provided device identities are accepted by the CLI.
"""
import argparse
import array
import fcntl
import hashlib
import os
import re
import stat
import sys
from pathlib import Path

SECTOR = 512
GEOMETRY = {
    "mmcblk1p1": (4098, 2048000),
    "mmcblk1p2": (2052098, 3072000),
    "mmcblk1p3": (5124098, 1024000),
}
USER_SECTORS = 7634944
PRE_SECTORS = 4098
BOOT_SECTORS = 8192
VIRTUAL_FS = {"proc", "sysfs", "tmpfs", "devtmpfs", "devpts", "cgroup",
              "cgroup2", "pstore", "securityfs", "debugfs", "tracefs",
              "configfs", "mqueue", "hugetlbfs", "bpf", "fusectl",
              "autofs", "nsfs", "efivarfs", "binfmt_misc", "functionfs"}


class SafetyError(Exception):
    pass


def require(condition, message):
    if not condition:
        raise SafetyError(message)


def sys_block(dev, sysroot=Path("/sys")):
    path = sysroot / "dev/block" / ("%d:%d" % dev)
    try:
        path = path.resolve(strict=True)
        require((path / "dev").read_text().strip() == "%d:%d" % dev,
                "sysfs device identity mismatch: %s" % (dev,))
        return path
    except (OSError, RuntimeError) as exc:
        raise SafetyError("unresolved block device %s: %s" % (dev, exc))


def physical_devices(dev, sysroot=Path("/sys"), seen=None):
    """Return whole-device sysfs paths; reject cycles, empty mappings and unknowns."""
    seen = set() if seen is None else seen
    require(dev not in seen, "cyclic block-device topology")
    seen = seen | {dev}
    path = sys_block(dev, sysroot)
    if (path / "partition").exists():
        parent = path.parent
        try:
            parent_dev = tuple(map(int, (parent / "dev").read_text().strip().split(":")))
        except (OSError, ValueError):
            raise SafetyError("cannot resolve partition parent: %s" % path)
        require(len(parent_dev) == 2, "invalid parent device")
        return physical_devices(parent_dev, sysroot, seen)

    slaves = path / "slaves"
    require(slaves.is_dir(), "missing sysfs slaves directory: %s" % path)
    children = list(slaves.iterdir())
    if children:
        result = set()
        for child in children:
            try:
                child_dev = tuple(map(int, (child.resolve(strict=True) / "dev").read_text().strip().split(":")))
            except (OSError, ValueError, RuntimeError):
                raise SafetyError("cannot resolve backing device: %s" % child)
            require(len(child_dev) == 2, "invalid backing device")
            result.update(physical_devices(child_dev, sysroot, seen))
        return result

    # Virtual devices without a verifiable backing stack are not proof of safety.
    require("/virtual/" not in str(path), "unresolved virtual block device: %s" % path)
    require(bool(re.fullmatch(r"(mmcblk[0-9]+|sd[a-z]+|nvme[0-9]+n[0-9]+|vd[a-z]+)", path.name)),
            "unrecognized physical block device: %s" % path)
    return {path}


def parse_mountinfo(lines):
    result = []
    for line in lines:
        fields = line.rstrip("\n").split(" ")
        try:
            sep = fields.index("-")
            require(sep >= 6 and len(fields) >= sep + 4, "malformed mountinfo")
            dev = tuple(map(int, fields[2].split(":")))
            require(len(dev) == 2, "malformed mount device")
            result.append((dev, set(fields[5].split(",")), fields[sep + 1], fields[4]))
        except (ValueError, IndexError):
            raise SafetyError("malformed mountinfo")
    require(bool(result), "empty mountinfo")
    return result


def assert_no_rw_emmc(mounts, emmc, sysroot=Path("/sys")):
    for dev, opts, fs, mountpoint in mounts:
        if "rw" not in opts:
            continue
        require("ro" not in opts, "ambiguous mount options: %s" % mountpoint)
        path = sysroot / "dev/block" / ("%d:%d" % dev)
        if not path.exists():
            # Overlay/FUSE can conceal a writable block-backed lower/upper layer.
            require(dev[0] == 0 and fs in VIRTUAL_FS,
                    "unknown writable mount backing: %s (%s)" % (mountpoint, fs))
            continue
        require(emmc not in physical_devices(dev, sysroot),
                "eMMC or its partition mounted rw: %s" % mountpoint)


def device_info(path):
    st = os.stat(path)
    require(stat.S_ISBLK(st.st_mode), "not a block device: %s" % path)
    return (os.major(st.st_rdev), os.minor(st.st_rdev))


def read_number(path):
    try:
        value = path.read_text().strip()
        require(value.isdigit(), "invalid sysfs value: %s" % path)
        return int(value)
    except OSError as exc:
        raise SafetyError("cannot read sysfs %s: %s" % (path, exc))


def validate_devices():
    emmc_dev = device_info("/dev/mmcblk1")
    emmc_path = sys_block(emmc_dev)
    require(emmc_path.name == "mmcblk1" and not (emmc_path / "partition").exists(),
            "unexpected eMMC identity")
    require(physical_devices(emmc_dev) == {emmc_path}, "eMMC is not a physical leaf")
    require(read_number(emmc_path / "size") == USER_SECTORS, "unexpected eMMC size")
    result = {}
    for name, (start, size) in GEOMETRY.items():
        dev = device_info("/dev/" + name)
        part = sys_block(dev)
        require(part.name == name and part.parent == emmc_path,
                "partition identity/parent mismatch: %s" % name)
        require(read_number(part / "start") == start and read_number(part / "size") == size,
                "partition geometry mismatch: %s" % name)
        result[name] = (dev, size * SECTOR)
    for name in ("mmcblk1boot0", "mmcblk1boot1"):
        dev = device_info("/dev/" + name)
        path = sys_block(dev)
        require(path.name == name and emmc_path in path.parents,
                "boot hardware area identity mismatch: %s" % name)
        require(read_number(path / "size") == BOOT_SECTORS,
                "boot hardware area size mismatch: %s" % name)
        result[name] = (dev, BOOT_SECTORS * SECTOR)
    return emmc_path, emmc_dev, result


def assert_readonly(path, expected_dev):
    """Full backups require the kernel block-device read-only flag as well."""
    fd = os.open(path, os.O_RDONLY | getattr(os, "O_CLOEXEC", 0))
    try:
        st = os.fstat(fd)
        require(stat.S_ISBLK(st.st_mode) and
                (os.major(st.st_rdev), os.minor(st.st_rdev)) == expected_dev,
                "source device changed: %s" % path)
        value = array.array("i", [0])
        fcntl.ioctl(fd, 0x125e, value, True)  # Linux BLKROGET
        require(value[0] == 1, "source block device is not read-only: %s" % path)
    finally:
        os.close(fd)


def approved_destination(dest, emmc, mounts, sysroot=Path("/sys")):
    require(not dest.exists() and not dest.is_symlink(),
            "destination must be a new directory")
    parent = dest.parent
    require(parent.is_dir(), "destination parent must already exist")
    parent = parent.resolve(strict=True)
    st = os.stat(parent)
    dev = (os.major(st.st_dev), os.minor(st.st_dev))
    require(any(m[0] == dev for m in mounts), "destination absent from mountinfo")
    require(emmc not in physical_devices(dev, sysroot),
            "destination is backed by protected eMMC")
    return parent, dev


def copy_exact(source, filename, offset, length, directory_fd, expected_dev, expected_dest_dev):
    flags = os.O_RDONLY | getattr(os, "O_CLOEXEC", 0)
    fd = os.open(source, flags)
    try:
        st = os.fstat(fd)
        require(stat.S_ISBLK(st.st_mode) and (os.major(st.st_rdev), os.minor(st.st_rdev)) == expected_dev,
                "source device changed: %s" % source)
        os.lseek(fd, offset, os.SEEK_SET)
        out = os.open(filename, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                      getattr(os, "O_NOFOLLOW", 0), 0o600, dir_fd=directory_fd)
        digest = hashlib.sha256()
        try:
            require(os.fstat(out).st_dev == expected_dest_dev, "destination changed filesystem")
            remaining = length
            while remaining:
                data = os.read(fd, min(1024 * 1024, remaining))
                require(bool(data), "short read from %s" % source)
                digest.update(data)
                view = memoryview(data)
                while view:
                    n = os.write(out, view)
                    require(n > 0, "short write to %s" % filename)
                    view = view[n:]
                remaining -= len(data)
            os.fsync(out)
            require(os.fstat(out).st_size == length, "incorrect backup length: %s" % filename)
        finally:
            os.close(out)
        verify = os.open(filename, os.O_RDONLY | getattr(os, "O_NOFOLLOW", 0), dir_fd=directory_fd)
        try:
            check = hashlib.sha256()
            while True:
                data = os.read(verify, 1024 * 1024)
                if not data:
                    break
                check.update(data)
            require(check.digest() == digest.digest(), "destination verification failed: %s" % filename)
        finally:
            os.close(verify)
        return digest.hexdigest()
    finally:
        os.close(fd)


def run(dest, mode):
    emmc, emmc_dev, parts = validate_devices()
    mounts = parse_mountinfo(Path("/proc/self/mountinfo").read_text().splitlines())
    parent, dest_dev = approved_destination(dest, emmc, mounts)
    if mode == "full":
        assert_no_rw_emmc(mounts, emmc)
        assert_readonly("/dev/mmcblk1", emmc_dev)
        for name, (dev, _) in parts.items():
            assert_readonly("/dev/" + name, dev)

    # Existing user-area geometry is contiguous; preserve the tail as well.
    tasks = [("/dev/mmcblk1", "emmc-reserved-pre-p1.bin", 0,
              PRE_SECTORS * SECTOR, emmc_dev)]
    if mode == "full":
        for name in GEOMETRY:
            tasks.append(("/dev/" + name, "emmc-" + name[7:] + ".img",
                          0, parts[name][1], parts[name][0]))
        tail_start = GEOMETRY["mmcblk1p3"][0] + GEOMETRY["mmcblk1p3"][1]
        tasks.append(("/dev/mmcblk1", "emmc-tail.img", tail_start * SECTOR,
                      (USER_SECTORS - tail_start) * SECTOR, emmc_dev))
        for name in ("mmcblk1boot0", "mmcblk1boot1"):
            tasks.append(("/dev/" + name, "emmc-" + name[7:] + ".img",
                          0, parts[name][1], parts[name][0]))

    required = sum(task[3] for task in tasks)
    fs = os.statvfs(parent)
    require(fs.f_bavail * fs.f_frsize >= required, "insufficient destination space")

    os.mkdir(str(parent / dest.name), 0o700)
    actual = parent / dest.name
    try:
        st = os.stat(actual)
        require((os.major(st.st_dev), os.minor(st.st_dev)) == dest_dev,
                "destination filesystem changed")
        dfd = os.open(str(actual), os.O_RDONLY | getattr(os, "O_DIRECTORY", 0) |
                      getattr(os, "O_NOFOLLOW", 0))
        try:
            hashes = []
            for source, name, offset, length, dev in tasks:
                if mode == "full":
                    current_mounts = parse_mountinfo(
                        Path("/proc/self/mountinfo").read_text().splitlines())
                    assert_no_rw_emmc(current_mounts, emmc)
                    assert_readonly(source, dev)
                print("Reading %s -> %s (%d bytes)" % (source, name, length), flush=True)
                digest = copy_exact(source, name, offset, length, dfd, dev, st.st_dev)
                hashes.append("%s  %s\n" % (digest, name))
            if mode == "full":
                assert_no_rw_emmc(parse_mountinfo(
                    Path("/proc/self/mountinfo").read_text().splitlines()), emmc)
                assert_readonly("/dev/mmcblk1", emmc_dev)
                for name, (dev, _) in parts.items():
                    assert_readonly("/dev/" + name, dev)
            fd = os.open("SHA256SUMS", os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                         getattr(os, "O_NOFOLLOW", 0), 0o600, dir_fd=dfd)
            try:
                data = "".join(hashes).encode("ascii")
                while data:
                    n = os.write(fd, data)
                    require(n > 0, "short write of SHA256SUMS")
                    data = data[n:]
                os.fsync(fd)
            finally:
                os.close(fd)
            os.fsync(dfd)
        finally:
            os.close(dfd)
    except Exception:
        print("ERROR: incomplete backup remains at %s; do not use it for recovery" % actual,
              file=sys.stderr)
        raise
    print("Backup and read-back SHA-256 verification completed: %s" % actual)
    print("This does not establish that recovery has been tested.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--dest", required=True, help="new directory on proven external block storage")
    parser.add_argument("--mode", choices=("metadata", "full"), default="metadata")
    args = parser.parse_args()
    try:
        run(Path(args.dest).absolute(), args.mode)
    except (SafetyError, OSError, ValueError) as exc:
        print("ERROR: %s" % exc, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
