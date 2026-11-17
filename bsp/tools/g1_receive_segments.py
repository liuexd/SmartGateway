#!/usr/bin/env python3
"""Receive G1 segments over pinned-key SSH to a new host directory.

This does not boot a recovery system or write any target block device.  A G1
archive is complete only after all source guards, transport checks, fsync,
readback, checksums and the audited MBR verifier succeed.
"""
import argparse
import hashlib
import ipaddress
import os
from pathlib import Path
import re
import select
import stat
import subprocess
import sys
import threading

import g1_verify_segments as archive


class ReceiveError(Exception):
    pass


def require(value, message):
    if not value:
        raise ReceiveError(message)


def drain_errors(pipe, store):
    while True:
        chunk = pipe.read(4096)
        if not chunk:
            break
        if len(store) < 65536:
            store.extend(chunk[:65536-len(store)])


def receive_one(command, dfd, name, length, idle_timeout=120):
    fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                 getattr(os, 'O_CLOEXEC', 0) | getattr(os, 'O_NOFOLLOW', 0),
                 0o600, dir_fd=dfd)
    process = None
    errors = bytearray()
    try:
        require(os.fstat(fd).st_dev == os.fstat(dfd).st_dev, 'destination filesystem changed')
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, bufsize=0)
        reader = threading.Thread(target=drain_errors, args=(process.stderr, errors), daemon=True)
        reader.start()
        digest, remaining = hashlib.sha256(), length
        while True:
            ready, _, _ = select.select([process.stdout], [], [], idle_timeout)
            require(bool(ready), 'transport stalled: ' + name)
            data = os.read(process.stdout.fileno(), min(1024*1024, remaining + 1))
            if not data:
                break
            require(len(data) <= remaining, 'source sent excess bytes: ' + name)
            remaining -= len(data)
            digest.update(data)
            view = memoryview(data)
            while view:
                n = os.write(fd, view)
                require(n > 0, 'short destination write: ' + name)
                view = view[n:]
        try:
            status = process.wait(timeout=idle_timeout)
        except subprocess.TimeoutExpired as exc:
            raise ReceiveError('transport did not terminate: ' + name) from exc
        reader.join(timeout=5)
        require(not reader.is_alive(), 'transport stderr did not close: ' + name)
        text = errors.decode('utf-8', errors='replace')
        require(remaining == 0, 'short transport stream: ' + name + ': ' + text[-500:])
        markers = re.findall(r'^G1_SHA256=([0-9a-f]{64})\s*$', text, re.MULTILINE)
        require(status == 0 and len(markers) == 1 and markers[0] == digest.hexdigest(),
                'source rejected or digest disagrees: ' + name + ': ' + text[-500:])
        os.fsync(fd)
        require(os.fstat(fd).st_size == length, 'destination size changed: ' + name)
        os.close(fd)
        fd = -1
        verify_fd, before = archive.open_regular(dfd, name)
        try:
            actual = hashlib.sha256()
            while True:
                chunk = os.read(verify_fd, 1024*1024)
                if not chunk:
                    break
                actual.update(chunk)
            require(archive.unchanged(before, os.fstat(verify_fd)) and
                    actual.digest() == digest.digest(), 'destination readback mismatch: ' + name)
        finally:
            os.close(verify_fd)
        return digest.hexdigest()
    finally:
        if fd >= 0:
            os.close(fd)
        if process is not None:
            if process.poll() is None:
                process.kill()
                process.wait()
            if process.stdout: process.stdout.close()
            if process.stderr: process.stderr.close()


def checked_file(path, private=False):
    st = os.lstat(path)
    require(stat.S_ISREG(st.st_mode) and not stat.S_ISLNK(st.st_mode), 'untrusted key path')
    require(st.st_uid == os.geteuid() and not (st.st_mode & 0o022), 'key file ownership or permissions unsafe')
    if private:
        require(not (st.st_mode & 0o077), 'identity key permissions too open')


def receive(destination, address, port, identity, known_hosts):
    ipaddress.IPv4Address(address)
    require(1 <= port <= 65535, 'invalid SSH port')
    checked_file(identity, private=True)
    checked_file(known_hosts)
    tasks, _ = archive.layout()
    parent = destination.parent.resolve(strict=True)
    require(not destination.exists() and not destination.is_symlink(), 'destination already exists')
    free = os.statvfs(parent)
    require(free.f_bavail * free.f_frsize >= sum(n for _, n in tasks), 'insufficient destination space')
    os.mkdir(parent / destination.name, 0o700)
    dfd = os.open(parent / destination.name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    manifest_created = False
    try:
        checksums = []
        for name, size in tasks:
            command = ['ssh', '-F', '/dev/null', '-T', '-p', str(port), '-i', str(identity),
                       '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes', '-o', 'StrictHostKeyChecking=yes',
                       '-o', 'UserKnownHostsFile=' + str(known_hosts), '-o', 'GlobalKnownHostsFile=/dev/null',
                       '-o', 'ConnectTimeout=15', '-o', 'ServerAliveInterval=15', '-o', 'ServerAliveCountMax=3',
                       'root@' + address, 'python3 -u /usr/bin/g1_stream_segment.py ' + name]
            print('Receiving %s (%d bytes)' % (name, size), flush=True)
            digest = receive_one(command, dfd, name, size)
            checksums.append('%s  %s\n' % (digest, name))
        mf = os.open('SHA256SUMS', os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=dfd)
        manifest_created = True
        try:
            data = ''.join(checksums).encode('ascii')
            while data:
                n = os.write(mf, data)
                require(n > 0, 'short checksum write')
                data = data[n:]
            os.fsync(mf)
        finally:
            os.close(mf)
        os.fsync(dfd)
        archive.verify(parent / destination.name)
        os.fsync(dfd)
        print('Archive internally verified; recovery boot and restoration are separate requirements.')
    except Exception:
        if manifest_created:
            os.unlink('SHA256SUMS', dir_fd=dfd)
            os.fsync(dfd)
        print('ERROR: incomplete backup remains; do not use for recovery.', file=sys.stderr)
        raise
    finally:
        os.close(dfd)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dest', type=Path, required=True, help='new directory on the receiving host')
    parser.add_argument('--address', required=True, help='verified IPv4 address of RAM recovery')
    parser.add_argument('--port', type=int, required=True)
    parser.add_argument('--identity', type=Path, required=True, help='private key for the temporary recovery SSH service')
    parser.add_argument('--known-hosts', type=Path, required=True, help='pinned recovery host key')
    args = parser.parse_args()
    try:
        receive(args.dest.absolute(), args.address, args.port,
                args.identity.absolute(), args.known_hosts.absolute())
    except (ReceiveError, archive.VerificationError, OSError, ValueError) as exc:
        print('ERROR: ' + str(exc), file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
