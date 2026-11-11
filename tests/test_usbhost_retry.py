# SPDX-License-Identifier: MIT
"""Exercise the patched TinyUSB stack with a deterministic fake HCD, without MMIO."""
from pathlib import Path
import importlib.util, re, subprocess, tempfile, unittest
ROOT=Path(__file__).resolve().parents[1]
class HostRetry(unittest.TestCase):
    def test_real_host_stack(self):
        if not (ROOT/"third_party/tinyusb/.fetched-0.21.0").exists():
            self.skipTest("make deps provides pinned TinyUSB for the real-stack test")
        spec=importlib.util.spec_from_file_location('overlay',ROOT/'scripts/usb_host_overlay.py')
        overlay=importlib.util.module_from_spec(spec);spec.loader.exec_module(overlay)
        with tempfile.TemporaryDirectory(prefix='usb-retry-') as d:
            overlay.generate(d)
            # Compile the real driver time adapter too, with only micros() replaced.
            clock=re.search(r'uint32_t tusb_time_millis_api\(void\) \{.*?\n\}',
                            (ROOT/'drivers/usb_host.c').read_text(),re.S).group(0)
            (Path(d)/'host-clock.h').write_text('static uint32_t raw_us;\nstatic uint32_t micros(void) {return raw_us;}\n'+
                clock.replace('tusb_time_millis_api','host_clock_from_micros'))
            exe=Path(d)/'test'
            subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                '-DUSBHOST_CLOCK_TEST=1','-DUSB_ROLE_HOST=1','-Iinclude','-Ithird_party/tinyusb/src','-I'+d,'-Ithird_party/tinyusb/src/host',
                'tests/usbhost_retry.c','third_party/tinyusb/src/common/tusb_fifo.c',
                'third_party/tinyusb/src/tusb.c',str(Path(d)/'hid_host.c'),'core/usbhost_stats.c','core/usblog.c','-o',str(exe)],cwd=ROOT,check=True)
            subprocess.run([str(exe)],cwd=ROOT,check=True,timeout=10)
