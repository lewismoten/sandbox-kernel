"""Host-role keyboard glue with fakes; no USB hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class UsbKbdTests(unittest.TestCase):
    def test_host_glue(self):
        with tempfile.TemporaryDirectory(prefix='usbkbd-') as d:
            exe = str(Path(d) / "usbkbd")
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            "core/hidboot.c", "core/usbkbd.c", "tests/usbkbd.c", "-o", exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

if __name__ == '__main__':
    unittest.main(verbosity=2)
