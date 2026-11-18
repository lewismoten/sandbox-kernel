"""Report descriptor parser and report decoder for a dongle's mouse/consumer interface; no hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HidRepTests(unittest.TestCase):
    def test_mouse_system_consumer(self):
        with tempfile.TemporaryDirectory(prefix='hidrep-') as d:
            exe = str(Path(d) / "hidrep")
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            "core/hidrep.c", "tests/hidrep.c", "-o", exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

if __name__ == '__main__':
    unittest.main(verbosity=2)
