#!/usr/bin/env python3
"""Validate G1 segmented backups without writing an image or block device.

Checks the audited user-area layout, every SHA256SUMS entry and both boot areas.
Success proves internal archive consistency, not independent boot or restoration.
"""
import argparse
import hashlib
import os
import re
import stat
import struct
import sys
from pathlib import Path

import g1_emmc_backup_verify as source


class VerificationError(Exception):
    pass


def check(ok, message):
    if not ok:
        raise VerificationError(message)


def open_regular(dfd, name):
    fd = os.open(name, os.O_RDONLY | getattr(os, 'O_CLOEXEC', 0) |
                 getattr(os, 'O_NOFOLLOW', 0), dir_fd=dfd)
    try:
        st = os.fstat(fd)
        check(stat.S_ISREG(st.st_mode), 'not a regular file: ' + name)
        return fd, st
    except Exception:
        os.close(fd)
        raise


def unchanged(before, after):
    return (before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns,
            before.st_ctime_ns) == (after.st_dev, after.st_ino, after.st_size,
                                     after.st_mtime_ns, after.st_ctime_ns)


def layout():
    parts = source.GEOMETRY
    tasks = [('emmc-reserved-pre-p1.bin', source.PRE_SECTORS * source.SECTOR)]
    pos = source.PRE_SECTORS
    for dev, (start, sectors) in parts.items():
        check(start == pos, 'non-contiguous partition layout')
        tasks.append(('emmc-' + dev[7:] + '.img', sectors * source.SECTOR))
        pos = start + sectors
    check(pos <= source.USER_SECTORS, 'partition exceeds user area')
    tasks.append(('emmc-tail.img', (source.USER_SECTORS - pos) * source.SECTOR))
    user_count = len(tasks)
    tasks.extend((('emmc-boot0.img', source.BOOT_SECTORS * source.SECTOR),
                  ('emmc-boot1.img', source.BOOT_SECTORS * source.SECTOR)))
    return tasks, user_count


def verify_mbr(header):
    check(len(header) >= 512 and header[510:512] == b'\x55\xaa', 'invalid MBR signature')
    types = (0x83, 0x83, 0x0c)
    for i, ((_, (start, size)), typ) in enumerate(zip(source.GEOMETRY.items(), types)):
        entry = header[446 + 16*i:462 + 16*i]
        got_start, got_size = struct.unpack_from('<II', entry, 8)
        check(entry[4] == typ and (got_start, got_size) == (start, size),
              'MBR partition %d does not match audited baseline' % (i + 1))
    check(header[446 + 3*16:446 + 4*16] == bytes(16), 'unexpected fourth MBR partition')


def verify(directory):
    tasks, user_count = layout()
    names = {name for name, _ in tasks}
    dfd = os.open(directory, os.O_RDONLY | getattr(os, 'O_DIRECTORY', 0) |
                  getattr(os, 'O_NOFOLLOW', 0))
    try:
        mfd, mst = open_regular(dfd, 'SHA256SUMS')
        try:
            check(0 < mst.st_size <= 4096, 'invalid SHA256SUMS size')
            raw = os.read(mfd, 4097)
            check(len(raw) == mst.st_size and unchanged(mst, os.fstat(mfd)),
                  'SHA256SUMS changed while reading')
        finally:
            os.close(mfd)
        try:
            lines = raw.decode('ascii').splitlines()
        except UnicodeError as exc:
            raise VerificationError('SHA256SUMS must be ASCII') from exc
        expected = {}
        for line in lines:
            match = re.fullmatch(r'([0-9a-f]{64})  ([A-Za-z0-9._-]+)', line)
            check(match is not None, 'malformed SHA256SUMS entry')
            digest, name = match.groups()
            check(name in names and name not in expected, 'unexpected or duplicate entry: ' + name)
            expected[name] = digest
        check(set(expected) == names, 'SHA256SUMS does not contain every required segment')
        user_digest = hashlib.sha256()
        for idx, (name, length) in enumerate(tasks):
            fd, before = open_regular(dfd, name)
            try:
                check(before.st_size == length, 'incorrect segment length: ' + name)
                digest, count = hashlib.sha256(), 0
                header = b''
                while True:
                    chunk = os.read(fd, 1024 * 1024)
                    if not chunk:
                        break
                    if idx == 0 and len(header) < 512:
                        header += chunk[:512-len(header)]
                    digest.update(chunk)
                    if idx < user_count:
                        user_digest.update(chunk)
                    count += len(chunk)
                check(count == length and unchanged(before, os.fstat(fd)),
                      'segment changed while reading: ' + name)
                check(digest.hexdigest() == expected[name], 'digest mismatch: ' + name)
                if idx == 0:
                    verify_mbr(header)
                print('OK %s (%d bytes)' % (name, length))
            finally:
                os.close(fd)
        print('User area SHA256 (ordered segments): ' + user_digest.hexdigest())
        print('Archive consistency only: boot and restoration remain unverified.')
    finally:
        os.close(dfd)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('directory', type=Path, help='directory containing a completed G1 full backup')
    args = p.parse_args()
    try:
        verify(args.directory)
    except (VerificationError, OSError, ValueError) as exc:
        print('ERROR: ' + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
