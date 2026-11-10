"""USB.TXT text logic (core/usblog.c) and the USBLOG client's joining of pieces."""
from pathlib import Path
import subprocess, sys, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

class UsbLogTests(unittest.TestCase):
    def test_text(self):
        with tempfile.TemporaryDirectory(prefix='usblog-') as d:
            exe = str(Path(d) / 'usblog')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            'core/usblog.c', 'tests/usblog.c', '-o', exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

    def test_client_joins_pieces(self):
        import usb_kernel
        self.assertEqual(usb_kernel.usblog_text(['a|b','c|']), 'a\nbc\n')

if __name__ == '__main__':
    unittest.main(verbosity=2)
