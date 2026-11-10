"""Boot record and Pi name text logic."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class BootInfoTests(unittest.TestCase):
    def test_text(self):
        with tempfile.TemporaryDirectory(prefix='bootinfo-') as d:
            exe = str(Path(d) / 'bootinfo')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            'core/bootinfo.c', 'tests/bootinfo.c', '-o', exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

if __name__ == '__main__':
    unittest.main(verbosity=2)
