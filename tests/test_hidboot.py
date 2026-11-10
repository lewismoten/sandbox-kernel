"""Real unlinked USB role decision, native assertions; no USB hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HidBootTests(unittest.TestCase):
    def test_descriptors_reports_and_keys(self):
        with tempfile.TemporaryDirectory(prefix='hidboot-') as d:
            exe = str(Path(d) / "hidboot")
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            "core/usbdesc.c", "core/hidboot.c", "tests/hidboot.c", "-o", exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

if __name__ == '__main__':
    unittest.main(verbosity=2)
