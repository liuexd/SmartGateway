#!/usr/bin/env python3
"""Read one fixed G1 eMMC segment to stdout for authenticated transport.

Intended for a minimal independently booted recovery system.  Only reads the
source devices, and refuses online/rw or unexpected device topology.  The host
must independently authenticate the transport, validate length and hash, fsync,
read back, and only then declare the backup complete.
"""
import hashlib
import os
import stat
import sys
from pathlib import Path

import g1_emmc_backup_verify as g1


def stream(name, output):
    emmc, emmc_dev, parts = g1.validate_devices()
    mounts = g1.parse_mountinfo(Path('/proc/self/mountinfo').read_text().splitlines())
    g1.assert_no_rw_emmc(mounts, emmc)
    devices = [('/dev/mmcblk1', emmc_dev)] + [('/dev/' + n, d) for n, (d, _) in parts.items()]
    for path, dev in devices:
        g1.assert_readonly(path, dev)
    tasks = {'emmc-reserved-pre-p1.bin': ('/dev/mmcblk1', 0, g1.PRE_SECTORS*g1.SECTOR, emmc_dev)}
    for part in g1.GEOMETRY:
        tasks['emmc-' + part[7:] + '.img'] = ('/dev/' + part, 0, parts[part][1], parts[part][0])
    tail_start = g1.GEOMETRY['mmcblk1p3'][0] + g1.GEOMETRY['mmcblk1p3'][1]
    tasks['emmc-tail.img'] = ('/dev/mmcblk1', tail_start*g1.SECTOR,
                              (g1.USER_SECTORS-tail_start)*g1.SECTOR, emmc_dev)
    for part in ('mmcblk1boot0', 'mmcblk1boot1'):
        tasks['emmc-' + part[7:] + '.img'] = ('/dev/' + part, 0, parts[part][1], parts[part][0])
    g1.require(name in tasks, 'unapproved segment name')
    path, offset, length, dev = tasks[name]
    fd = os.open(path, os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0))
    try:
        st = os.fstat(fd)
        g1.require(stat.S_ISBLK(st.st_mode) and (os.major(st.st_rdev), os.minor(st.st_rdev)) == dev,
                   'source device identity changed')
        os.lseek(fd, offset, os.SEEK_SET)
        digest, remaining = hashlib.sha256(), length
        while remaining:
            data = os.read(fd, min(1024*1024, remaining))
            g1.require(bool(data), 'short source read')
            digest.update(data)
            view = memoryview(data)
            while view:
                count = output.write(view)
                g1.require(count is not None and count > 0, 'short transport write')
                view = view[count:]
            remaining -= len(data)
        output.flush()
        g1.assert_no_rw_emmc(g1.parse_mountinfo(Path('/proc/self/mountinfo').read_text().splitlines()), emmc)
        for devpath, number in devices:
            g1.assert_readonly(devpath, number)
        return digest.hexdigest()
    finally:
        os.close(fd)


def main():
    if len(sys.argv) != 2:
        print('usage: g1_stream_segment.py SEGMENT', file=sys.stderr)
        return 2
    try:
        digest = stream(sys.argv[1], sys.stdout.buffer)
    except (g1.SafetyError, OSError, ValueError) as exc:
        print('ERROR: ' + str(exc), file=sys.stderr)
        return 1
    print('G1_SHA256=' + digest, file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main())
