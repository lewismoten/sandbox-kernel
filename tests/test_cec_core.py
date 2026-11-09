"""CEC message, key and state-machine cores built on the host, native assertions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class CecCoreTests(unittest.TestCase):
    def test_cec_msg_keys_sm(self):
        with tempfile.TemporaryDirectory(prefix='cec-core-') as d:
            exe = str(Path(d) / 'cec-core')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-Iinclude',
                            'drivers/cec_msg.c', 'drivers/cec_keys.c', 'drivers/cec_sm.c',
                            'tests/cec_core.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=30, capture_output=True, text=True).stdout
            self.assertEqual(out.count('PASS'), 3)

if __name__ == '__main__':
    unittest.main(verbosity=2)
