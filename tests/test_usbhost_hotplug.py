# SPDX-License-Identifier: MIT
"""Hub and keyboard hot-plug sequences over a scripted fake bus, with the real stack and kernel glue."""
from pathlib import Path
import importlib.util, re, subprocess, sys, tempfile, unittest
ROOT = Path(__file__).resolve().parents[1]

class HostHotplug(unittest.TestCase):
    def test_hot_plug_sequences(self):
        if not (ROOT/'third_party/tinyusb/.fetched-0.21.0').exists():
            self.skipTest('pinned TinyUSB required; no network in tests')
        spec = importlib.util.spec_from_file_location('host_overlay', ROOT/'scripts/usb_host_overlay.py')
        overlay = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(overlay)
        with tempfile.TemporaryDirectory(prefix='usb-hotplug-') as temp:
            dest = Path(temp)
            overlay.generate(dest)
            driver = (ROOT/'drivers/usb_host.c').read_text()
            def between(a, b): return driver[driver.index(a):driver.index(b)]
            task = re.search(r'void usb_task\(void\) \{.*?\n\}', driver, re.S).group(0)
            (dest/'host-driver.c').write_text('\n'.join([
                '#include "tusb.h"\n#include "usbkbd.h"\n#include "usblog.h"\n#include "usbhost_hooks.h"\n#include "usbhost_work.h"\n#include "host/hcd.h"',
                'uint32_t micros(void);uint64_t clock_us(void);void log_line(const char *s);bool usb_wait_failed(void);',
                'static bool initialized=true;static void watchdog(void) {}static void os_usb_work(uint64_t b,uint64_t e) {(void)b;(void)e;}',
                between('static void lg(', 'void usb_irq(void)'),       # log helper, device and configuration callbacks
                between('void tuh_mount_cb', 'bool usb_mounted('),      # mount/unmount, HID callbacks, arm
                between('void tuh_control_retry_cb', 'void usb_task('), # retry and enumeration log callbacks
                task]))
            exe = dest/'test'
            subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                '-Wno-unused-function', '-DHOST_TEST', '-DUSB_ROLE_HOST=1', '-Iinclude', '-Ithird_party/tinyusb/src',
                '-I'+temp, '-Ithird_party/tinyusb/src/host', 'tests/usbhost_hotplug.c', str(dest/'host-driver.c'), str(dest/'hid_host.c'),
                'core/usbhost_stats.c', 'core/usblog.c', 'core/usbkbd.c', 'core/hidboot.c', 'core/input.c',
                'third_party/tinyusb/src/common/tusb_fifo.c', 'third_party/tinyusb/src/tusb.c', '-o', str(exe)],
                cwd=ROOT, check=True)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=120)

if __name__ == '__main__':
    unittest.main(verbosity=2)
