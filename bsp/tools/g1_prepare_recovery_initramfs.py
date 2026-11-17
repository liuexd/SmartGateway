#!/usr/bin/env python3
"""Engineer a reproducible G1 RAM recovery initramfs from a staging rootfs.

Fixes the two manual gaps observed during the 2026-11-17 candidate-mini boot:
1. Unconditional devtmpfs mount when /dev is not already devtmpfs (static /dev/console
   previously skipped devtmpfs and hid /dev/mmcblk1*).
2. Absolute /usr/sbin/sshd path in g1_start_ssh.sh (avoiding sshd re-exec failure).
Also installs g1_set_emmc_ro.py and the verified G1 backup/restore tools without
adding any automatic eMMC mounts or writes.
"""
import argparse
import hashlib
import os
import shutil
import subprocess
import sys
from pathlib import Path

INIT_SCRIPT = """#!/bin/sh
# Engineered G1 RAM rescue shell: no automatic eMMC mounts, writes or reboot.
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PYTHONDONTWRITEBYTECODE=1
grep -q ' /proc proc ' /proc/mounts 2>/dev/null || mount -t proc proc /proc || echo "ERROR: proc mount failed"
grep -q ' /sys sysfs ' /proc/mounts 2>/dev/null || mount -t sysfs sysfs /sys || echo "ERROR: sysfs mount failed"
grep -q ' /dev devtmpfs ' /proc/mounts 2>/dev/null || mount -t devtmpfs devtmpfs /dev || echo "ERROR: devtmpfs mount failed"
mkdir -p /dev/pts /dev/shm /run /run/sshd
grep -q ' /dev/pts devpts ' /proc/mounts 2>/dev/null || mount -t devpts devpts /dev/pts 2>/dev/null || true
[ -c /dev/console ] || { echo "ERROR: no console"; exec /bin/sh; }
exec </dev/console >/dev/console 2>&1
printf '\\nG1 RAM recovery environment (engineered v2)\\n'
printf 'No eMMC write or automatic mount is performed.\\n'
printf 'Run /usr/bin/g1_set_emmc_ro.py to lock eMMC read-only before backup.\\n'
printf 'Run /usr/bin/g1_start_ssh.sh after configuring network via /bin/busybox ifconfig.\\n'
exec /bin/sh
"""

START_SSH_SCRIPT = """#!/bin/sh
set -eu
export PATH=/sbin:/bin:/usr/sbin:/usr/bin
export PYTHONDONTWRITEBYTECODE=1
python3 -B /usr/bin/g1_check_ram_root.py
umask 077
mkdir -p /run/sshd
if [ ! -f /run/g1-ssh-host-ed25519 ]; then
  /usr/bin/ssh-keygen -q -t ed25519 -N "" -f /run/g1-ssh-host-ed25519
fi
/usr/sbin/sshd -t -f /etc/ssh/g1_sshd_config
/usr/sbin/sshd -f /etc/ssh/g1_sshd_config
printf "Verify this one-time SSH host key over this serial console:\\n"
cat /run/g1-ssh-host-ed25519.pub
/usr/bin/ssh-keygen -lf /run/g1-ssh-host-ed25519.pub
"""

SET_RO_SCRIPT = """#!/usr/bin/env python3
\"\"\"Set Linux BLKROSET=1 on mmcblk1, its partitions, and boot areas, then verify.\"\"\"
import array
import fcntl
import os
import stat
import sys
from pathlib import Path

sys.path.insert(0, "/usr/bin")
import g1_emmc_backup_verify as g1

BLKROSET = 0x125D


def main():
    emmc, emmc_dev, parts = g1.validate_devices()
    mounts = g1.parse_mountinfo(Path("/proc/self/mountinfo").read_text().splitlines())
    g1.assert_no_rw_emmc(mounts, emmc)
    items = [("/dev/mmcblk1", emmc_dev)] + [("/dev/" + n, v[0]) for n, v in parts.items()]
    for path, dev in items:
        try:
            g1.assert_readonly(path, dev)
            continue
        except g1.SafetyError:
            pass
        fd = os.open(path, os.O_RDONLY)
        try:
            st = os.fstat(fd)
            g1.require(stat.S_ISBLK(st.st_mode) and
                       (os.major(st.st_rdev), os.minor(st.st_rdev)) == dev,
                       f"device identity changed: {path}")
            fcntl.ioctl(fd, BLKROSET, array.array("i", [1]), True)
        finally:
            os.close(fd)
        g1.assert_readonly(path, dev)
    for path, dev in items:
        g1.assert_readonly(path, dev)
    print("G1_ALL_BLOCK_DEVICES_READONLY")


if __name__ == "__main__":
    try:
        main()
    except (g1.SafetyError, OSError, ValueError) as exc:
        print("ERROR: " + str(exc), file=sys.stderr)
        sys.exit(1)
"""


def write_executable(path: Path, content: str):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    os.chmod(path, 0o755)


def prepare_rootfs(src_rootfs: Path, dst_rootfs: Path, tools_dir: Path):
    if dst_rootfs.exists():
        shutil.rmtree(dst_rootfs)
    shutil.copytree(src_rootfs, dst_rootfs, symlinks=True)

    write_executable(dst_rootfs / "init", INIT_SCRIPT)
    write_executable(dst_rootfs / "usr/bin/g1_start_ssh.sh", START_SSH_SCRIPT)
    write_executable(dst_rootfs / "usr/bin/g1_set_emmc_ro.py", SET_RO_SCRIPT)

    for tool_name in ("g1_emmc_backup_verify.py", "g1_stream_segment.py",
                      "g1_verify_segments.py", "g1_restore_segments.py"):
        src_tool = tools_dir / tool_name
        if not src_tool.is_file():
            raise FileNotFoundError(f"missing tool: {src_tool}")
        shutil.copy2(src_tool, dst_rootfs / "usr/bin" / tool_name)
        os.chmod(dst_rootfs / "usr/bin" / tool_name, 0o755)


def pack_cpio_gz(rootfs_dir: Path, out_cpio_gz: Path):
    out_cpio_gz.parent.mkdir(parents=True, exist_ok=True)
    find_proc = subprocess.Popen(
        ["find", ".", "-print0"],
        cwd=rootfs_dir,
        stdout=subprocess.PIPE,
    )
    sort_proc = subprocess.Popen(
        ["sort", "-z"],
        env={**os.environ, "LC_ALL": "C"},
        stdin=find_proc.stdout,
        stdout=subprocess.PIPE,
    )
    find_proc.stdout.close()
    cpio_proc = subprocess.Popen(
        ["cpio", "-o", "-H", "newc", "-0", "--quiet"],
        cwd=rootfs_dir,
        stdin=sort_proc.stdout,
        stdout=subprocess.PIPE,
    )
    sort_proc.stdout.close()
    with out_cpio_gz.open("wb") as gz_out:
        gzip_proc = subprocess.Popen(
            ["gzip", "-n", "-9"],
            stdin=cpio_proc.stdout,
            stdout=gz_out,
        )
        cpio_proc.stdout.close()
        rc_gz = gzip_proc.wait()
    rc_cpio = cpio_proc.wait()
    rc_sort = sort_proc.wait()
    rc_find = find_proc.wait()
    if any(rc != 0 for rc in (rc_find, rc_sort, rc_cpio, rc_gz)):
        raise RuntimeError(f"pack failed: find={rc_find} sort={rc_sort} cpio={rc_cpio} gzip={rc_gz}")


def write_boot_bundle(src_boot: Path, dst_rootfs: Path, dst_boot: Path, archive_name: str):
    dst_boot.mkdir(parents=True, exist_ok=True)
    for name in ("zImage", "100ask_imx6ull-14x14.dtb"):
        shutil.copy2(src_boot / name, dst_boot / name)
    archive_path = dst_boot / archive_name
    pack_cpio_gz(dst_rootfs, archive_path)
    lines = []
    for name in ("zImage", "100ask_imx6ull-14x14.dtb", archive_name):
        digest = hashlib.sha256((dst_boot / name).read_bytes()).hexdigest()
        lines.append(f"{digest}  {name}")
    (dst_boot / "SHA256SUMS").write_text("\n".join(lines) + "\n", encoding="ascii")
    return archive_path


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--src-rootfs", type=Path, required=True)
    p.add_argument("--src-boot", type=Path)
    p.add_argument("--dst-dir", type=Path, required=True)
    p.add_argument("--tools-dir", type=Path, default=Path(__file__).resolve().parent)
    p.add_argument("--archive-name", default="rescue-initramfs-v2.cpio.gz")
    args = p.parse_args()

    dst_rootfs = args.dst_dir / "rootfs"
    prepare_rootfs(args.src_rootfs, dst_rootfs, args.tools_dir)
    if args.src_boot:
        archive = write_boot_bundle(args.src_boot, dst_rootfs, args.dst_dir / "boot", args.archive_name)
        print(f"BUILT {archive} ({archive.stat().st_size} bytes)")
    else:
        archive = args.dst_dir / args.archive_name
        pack_cpio_gz(dst_rootfs, archive)
        print(f"BUILT {archive} ({archive.stat().st_size} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
