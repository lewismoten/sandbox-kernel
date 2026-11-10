"""Host-role keyboard review findings R1-R4 as regression tests; no hardware."""
from pathlib import Path
import re, subprocess, sys, tempfile, unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))

def preprocess(*defs):
    return subprocess.run(['cc', '-E', '-P', '-Iinclude', '-Itests/stubs', *defs, 'core/main.c'], cwd=ROOT,
                          check=True, capture_output=True, text=True).stdout

class HostRegress(unittest.TestCase):
    def test_r2_r3_r4_real_code(self):
        with tempfile.TemporaryDirectory(prefix='usbhost-') as d:
            exe = str(Path(d) / 'x')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-DHOST_TEST', '-Iinclude', '-Itests/stubs',
                            'core/hidboot.c', 'core/usbkbd.c', 'core/input.c', 'tests/usbhost_regress.c', '-o', exe], cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

    def test_r3_input_initialized_before_any_usb_servicing(self):
        src = (ROOT / 'core/main.c').read_text()
        self.assertEqual(src.count('input_init();'), 1)
        self.assertLess(src.index('input_init();'), src.index('usb_init();'))
        self.assertLess(src.index('input_init();'), src.index('store_init();'))

    def test_r4_arm_failure_removes_keyboard(self):
        src = (ROOT / 'drivers/usb_host.c').read_text()
        self.assertIn('usbkbd_poll_failed();', src)
        self.assertEqual(len(re.findall(r'tuh_hid_receive_report', src)), 1)  # every arm goes through the checked helper

    def test_r2_config_has_room_for_a_composite(self):
        cfg = (ROOT / 'include/tusb_config.h').read_text()
        self.assertGreaterEqual(int(re.search(r'#define CFG_TUH_HID (\d+)', cfg).group(1)), 3)
        self.assertGreaterEqual(int(re.search(r'#define CFG_TUH_ENDPOINT_MAX (\d+)', cfg).group(1)), 8)

    def test_r1_host_image_has_no_confirmation_path(self):
        host = preprocess('-DUSB_ROLE_HOST=1')
        device = preprocess('-DUSB_ROLE_HOST=0')
        self.assertNotIn('Trial confirmed', host)
        self.assertNotIn('control_commit(c)', host.split('Fallback commit failed')[1])  # only the fallback-clear commit precedes
        self.assertIn('Trial confirmed after 30 seconds', device)
        self.assertIn('not on trial or companion handoff', host)                  # a non-trial host boot does nothing
        self.assertNotIn('not on trial', device)
        self.assertIn('Companion metadata mismatch', host)
        self.assertNotIn('host_companion_launch()', host)
        self.assertIn('host_companion_launch()', device)
        self.assertIn('h[3]==1', host) # companion is never treated as a trial
        # the refusal comes before USB, SD or panel start-up
        self.assertLess(host.index('not on trial'), host.index('usb_init();'))

    def test_r1_assembler_refuses_host_runtime(self):
        import assemble_boot
        with tempfile.TemporaryDirectory(prefix='role-') as d:
            stamp = Path(d) / 'usb_role.stamp'
            stamp.write_text('host\n')
            with self.assertRaises(SystemExit) as e: assemble_boot.refuse_host_role(stamp)
            self.assertIn('USB_ROLE=host', str(e.exception))
            stamp.write_text('device\n'); assemble_boot.refuse_host_role(stamp)
            assemble_boot.refuse_host_role(Path(d) / 'missing')

if __name__ == '__main__':
    unittest.main(verbosity=2)
