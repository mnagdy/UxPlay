import os
from pathlib import Path
import socket
import subprocess
import tempfile
import threading
import unittest

HELPER = Path(__file__).resolve().parents[1] / 'scripts' / 'tv-debug'


class DebugHelperTest(unittest.TestCase):
    def test_missing_receiver(self):
        with tempfile.TemporaryDirectory(prefix='tv-') as home:
            result = subprocess.run([str(HELPER), 'on'], env=dict(os.environ, HOME=home), capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('not running', result.stderr)

    def test_round_trip_and_client_cleanup(self):
        with tempfile.TemporaryDirectory(prefix='tv-', dir='/tmp') as home:
            directory = Path(home) / '.uxplay-control'
            directory.mkdir(mode=0o700)
            with socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM) as server:
                server.bind(str(directory / 'control.sock'))
                server.settimeout(5)
                for command in ('on', 'off', 'status'):
                    observed = []
                    def reply():
                        message, address = server.recvfrom(32)
                        observed.append(message)
                        server.sendto(b'Debug overlay on', address)
                    thread = threading.Thread(target=reply)
                    thread.start()
                    result = subprocess.run([str(HELPER), command], env=dict(os.environ, HOME=home), capture_output=True, text=True)
                    thread.join(5)
                    self.assertEqual(observed, [command.encode()])
                    self.assertEqual(result.returncode, 0, result.stderr)
                    self.assertEqual(result.stdout.strip(), 'Debug overlay on')
                    self.assertEqual(list(directory.glob('client-*')), [])

    def test_insecure_directory_is_rejected(self):
        with tempfile.TemporaryDirectory(prefix='tv-') as home:
            directory = Path(home) / '.uxplay-control'
            directory.mkdir(mode=0o755)
            directory.chmod(0o755)
            result = subprocess.run([str(HELPER), 'off'], env=dict(os.environ, HOME=home), capture_output=True, text=True)
            self.assertEqual(result.returncode, 1)
            self.assertIn('Unsafe', result.stderr)


if __name__ == '__main__':
    unittest.main()
