"""CEC transport, EDID address, bounded log and timing words, built on the host against a fake register file."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class CecDrvTests(unittest.TestCase):
    def test_cec_transport(self):
        with tempfile.TemporaryDirectory(prefix='cec-drv-') as d:
            exe = str(Path(d) / 'cec-drv')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-Iinclude',
                            'drivers/cec_msg.c', 'drivers/cec_keys.c', 'drivers/cec_sm.c', 'drivers/cec_regs.c',
                            'drivers/cec_edid.c', 'drivers/cec_log.c', 'drivers/cec_drv.c',
                            'tests/cec_drv.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=30, capture_output=True, text=True).stdout
            self.assertEqual(out.count('PASS'), 14)

    def test_remote_every_button(self):
        """A TV remote pressing every button: every pass bounded, nothing loops or storms (tests/cec_buttons.c); the loop breadcrumb line."""
        with tempfile.TemporaryDirectory(prefix='cec-btn-') as d:
            exe = str(Path(d) / 'cec-buttons')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-Iinclude',
                            'drivers/cec_msg.c', 'drivers/cec_keys.c', 'drivers/cec_sm.c', 'drivers/cec_regs.c',
                            'drivers/cec_edid.c', 'drivers/cec_log.c', 'drivers/cec_drv.c', 'core/looptrace.c',
                            'tests/cec_buttons.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=60, capture_output=True, text=True).stdout
            self.assertEqual(out.count('PASS'), 5)

if __name__ == '__main__':
    unittest.main(verbosity=2)
