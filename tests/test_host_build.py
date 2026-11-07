# SPDX-License-Identifier: MIT
"""The shared host library must call its own kernel symbols, not libc namesakes."""
import pathlib
import subprocess
import sys
import tempfile
import unittest

import hostlib


class HostBuild(unittest.TestCase):
    def test_protocol_error_binds_to_harness(self):
        # Keep a bad binding's SIGSEGV/SystemExit out of the unittest runner.
        with tempfile.TemporaryDirectory() as work:
            library = hostlib.build(pathlib.Path(work) / 'core.so')
            result = subprocess.run([sys.executable, '-c', '''
import ctypes as C, sys
c = C.CDLL(sys.argv[1])
c.feed.argtypes = [C.c_char_p, C.c_uint]
c.feed(b'?\\n', 2)
c.protocol_task()
assert C.c_char_p.in_dll(c, 'last_error').value == b'command'
c.take_output.restype = C.c_char_p
assert c.take_output() == b'ERR command\\n'
print('harness error binding verified')
''', str(library)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('harness error binding verified', result.stdout)
