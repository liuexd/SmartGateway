import hashlib
import os
import pwd
import shlex
import shutil
import socket
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'bsp/tools'))
import g1_receive_segments as receiver


@unittest.skipUnless(Path('/usr/sbin/sshd').exists() and shutil.which('ssh-keygen') and shutil.which('ssh'),
                     'OpenSSH server/client are required for this host-only test')
class SshLoopbackTests(unittest.TestCase):
    def test_pinned_ssh_stream_and_wrong_host_key(self):
        with tempfile.TemporaryDirectory(prefix='sgw-g1-ssh-') as tmp:
            d = Path(tmp)
            host, user = d/'host', d/'user'
            for key in (host, user):
                subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', '', '-f', str(key)], check=True)
            auth = d/'authorized'
            auth.write_bytes((d/'user.pub').read_bytes())
            with socket.socket() as sock:
                sock.bind(('127.0.0.1', 0))
                port = sock.getsockname()[1]
            config = d/'sshd_config'
            config.write_text('Port %d\nListenAddress 127.0.0.1\nHostKey %s\nPidFile %s\n'
                              'AuthorizedKeysFile %s\nUsePAM no\nPasswordAuthentication no\n'
                              'KbdInteractiveAuthentication no\nPubkeyAuthentication yes\n'
                              'StrictModes no\nLogLevel ERROR\n' % (port, host, d/'pid', auth))
            known = d/'known_hosts'
            pub = (d/'host.pub').read_text().split()
            known.write_text('[127.0.0.1]:%d %s %s\n' % (port, pub[0], pub[1]))
            server = subprocess.Popen(['/usr/sbin/sshd', '-D', '-e', '-f', str(config)],
                                      stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
            try:
                for _ in range(40):
                    if server.poll() is not None:
                        self.fail('isolated sshd failed: ' + server.stderr.read().decode())
                    try:
                        with socket.create_connection(('127.0.0.1', port), timeout=.1): break
                    except OSError: time.sleep(.1)
                else: self.fail('isolated sshd did not listen')
                data = b'G1 isolated ssh transfer'
                digest = hashlib.sha256(data).hexdigest()
                code = ('import sys;sys.stdout.buffer.write(%r);sys.stdout.flush();'
                        'sys.stderr.write("G1_SHA256=%s\\n")') % (data, digest)
                account = pwd.getpwuid(os.geteuid()).pw_name
                command = ['ssh', '-F', '/dev/null', '-T', '-p', str(port), '-i', str(user),
                           '-o', 'BatchMode=yes', '-o', 'IdentitiesOnly=yes',
                           '-o', 'StrictHostKeyChecking=yes', '-o', 'UserKnownHostsFile='+str(known),
                           '-o', 'GlobalKnownHostsFile=/dev/null', account+'@127.0.0.1',
                           'python3 -c '+shlex.quote(code)]
                dfd = os.open(d, os.O_RDONLY | os.O_DIRECTORY)
                try:
                    self.assertEqual(receiver.receive_one(command, dfd, 'received', len(data), 10), digest)
                    self.assertEqual((d/'received').read_bytes(), data)
                    wrong = (d/'user.pub').read_text().split()
                    known.write_text('[127.0.0.1]:%d %s %s\n' % (port, wrong[0], wrong[1]))
                    with self.assertRaises(receiver.ReceiveError):
                        receiver.receive_one(command, dfd, 'bad-key', len(data), 10)
                finally: os.close(dfd)
            finally:
                server.terminate()
                try: server.wait(timeout=5)
                except subprocess.TimeoutExpired: server.kill(); server.wait()
                server.stderr.close()

if __name__ == '__main__': unittest.main()
