# SPDX-License-Identifier: MIT
"""Real patched host/HID/hub and kernel callbacks over an expensive fake HCD."""
from pathlib import Path
import importlib.util
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HostPolling(unittest.TestCase):
    def test_two_tv_keyboards(self):
        if not (ROOT/'third_party/tinyusb/.fetched-0.21.0').exists():
            self.skipTest('pinned TinyUSB required; no network in tests')
        spec = importlib.util.spec_from_file_location('host_overlay', ROOT/'scripts/usb_host_overlay.py')
        overlay = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(overlay)
        with tempfile.TemporaryDirectory(prefix='usb-poll-') as temp:
            dest = Path(temp)
            overlay.generate(dest)
            driver = (ROOT/'drivers/usb_host.c').read_text()
            config = re.search(r'bool tuh_enum_descriptor_configuration_cb\(.*?\n\}', driver, re.S).group(0)
            first = driver.index('static void arm(')
            last = driver.index('bool usb_mounted(', first)
            (dest/'host-keyboard.c').write_text('#include "tusb.h"\n#include "usbkbd.h"\n#include "usblog.h"\nvoid log_line(const char *s);\nstatic void lg(const char *s) {usblog_add(s); }\n'+config+'\n'+driver[first:last])
            task = re.search(r'void usb_task\(void\) \{.*?\n\}', driver, re.S).group(0)
            (dest/'host-task.h').write_text(task)
            exe = dest/'test'
            subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-DHOST_TEST', '-DUSB_ROLE_HOST=1', '-Iinclude',
                '-Ithird_party/tinyusb/src', '-I'+temp, '-Ithird_party/tinyusb/src/host',
                'tests/usbhost_poll.c', str(dest/'host-keyboard.c'), str(dest/'hid_host.c'), 'core/usbhost_stats.c',
                'core/usblog.c', 'core/usbkbd.c', 'core/hidboot.c', 'core/input.c',
                'third_party/tinyusb/src/common/tusb_fifo.c', 'third_party/tinyusb/src/tusb.c',
                '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=15)

    def test_dwc2_nak_deadlines(self):
        if not (ROOT/'third_party/tinyusb/.fetched-0.21.0').exists():
            self.skipTest('pinned TinyUSB required; no network in tests')
        spec = importlib.util.spec_from_file_location('bounded_overlay', ROOT/'scripts/usb_bounded_overlay.py')
        overlay = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(overlay)
        import sys
        with tempfile.TemporaryDirectory(prefix='usb-dwc2-poll-') as temp:
            dest = Path(temp)/'src'
            overlay.generate(dest)
            exe = Path(temp)/'test'
            subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-parameter', '-Wno-pointer-to-int-cast', '-Wno-int-to-pointer-cast',
                '-ffunction-sections', '-fdata-sections',
                *(['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']),
                '-DHOST_TEST', '-DUSB_ROLE_HOST=1', '-I'+str(dest), '-Iinclude',
                'tests/usbhost_dwc2_poll.c', 'drivers/usb_wait.c', 'core/usblog.c',
                str(dest/'common/tusb_fifo.c'), str(dest/'tusb.c'), '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=15)

    def test_counter_windows(self):
        with tempfile.TemporaryDirectory(prefix='usb-stats-') as temp:
            exe = Path(temp)/'test'
            subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror', '-Iinclude',
                'tests/usbhost_stats.c', 'core/usbhost_stats.c', 'core/usblog.c', '-o', str(exe)], cwd=ROOT, check=True)
            subprocess.run([str(exe)], cwd=ROOT, check=True, timeout=5)
