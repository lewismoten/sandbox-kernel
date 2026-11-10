"""Real unlinked USB role decision, native assertions; no USB hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class UsbRoleTests(unittest.TestCase):
    def test_role_decision(self):
        with tempfile.TemporaryDirectory(prefix='usbrole-') as d:
            exe = str(Path(d) / 'usbrole')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            'core/usbrole.c', 'tests/usbrole.c', '-o', exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

if __name__ == '__main__':
    unittest.main(verbosity=2)
