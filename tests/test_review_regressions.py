# SPDX-License-Identifier: MIT
"""Confirmed review findings: production HID admission, handle lifetime, serial totals."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import hostlib

ROOT = Path(__file__).resolve().parents[1]

class ReviewRegressions(unittest.TestCase):
    def run_probe(self, probe, sources=(), generated=None):
        with tempfile.TemporaryDirectory(prefix='review-') as tmp:
            work = Path(tmp)
            if generated:
                (work / generated[0]).write_text(generated[1])
            exe = work / 'probe'
            subprocess.run(['cc', '-std=c11', '-O1', '-fno-builtin', '-DHOST_TEST',
                            '-D_FORTIFY_SOURCE=0', '-Wall', '-Wextra', '-Werror',
                            '-Iinclude', '-Itests/stubs', '-I' + str(work),
                            *sources, 'tests/' + probe + '.c', '-o', str(exe)],
                           cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True, timeout=30)

    def test_hid_in_size_checked_before_endpoint_open(self):
        source = (ROOT / 'drivers/usb_host.c').read_text()
        start = source.index('bool tuh_enum_descriptor_configuration_cb(')
        end = source.index('\n}', start) + 2
        self.run_probe('usbhost_bounds', generated=('usbhost-bounds.h', source[start:end]))

    def test_handle_generation_crosses_16_bits_and_retires_at_exhaustion(self):
        self.run_probe('handle_generation', [s for s in hostlib.SOURCES if s != 'core/sys.c'])

    def test_serial_counters_cross_32_bits_saturate_and_keep_ring_wrap(self):
        self.run_probe('serial_counters')

if __name__ == '__main__':
    unittest.main()
