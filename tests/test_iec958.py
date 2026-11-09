"""IEC 958 subframe packer and HDMI N/CTS table, host-native (no hardware)."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class Iec958Tests(unittest.TestCase):
    def test_native(self):
        with tempfile.TemporaryDirectory(prefix='iec958-') as d:
            exe = str(Path(d) / 'iec958')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Iinclude', 'core/iec958.c', 'tests/iec958.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=60, capture_output=True, text=True).stdout
            self.assertIn('iec958 ok', out)

if __name__ == '__main__':
    unittest.main(verbosity=2)
