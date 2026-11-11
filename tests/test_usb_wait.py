# SPDX-License-Identifier: MIT
"""Run real HCD/DCD calls against a register file that never acknowledges."""
from pathlib import Path
import importlib.util
import subprocess
import sys
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class USBWait(unittest.TestCase):
    def test_stuck_registers(self):
        if not (ROOT/'third_party/tinyusb/.fetched-0.21.0').exists():
            self.skipTest('pinned TinyUSB is required; no fetching in tests')
        spec=importlib.util.spec_from_file_location('bounded',ROOT/'scripts/usb_bounded_overlay.py')
        overlay=importlib.util.module_from_spec(spec);spec.loader.exec_module(overlay)
        with tempfile.TemporaryDirectory(prefix='dwc2-wait-') as temp:
            dest=Path(temp)/'src';overlay.generate(dest)
            for host in (False,True):
                exe=Path(temp)/('host' if host else 'device')
                # The driver uses 32-bit DMA addresses on ARM; tests exercise PIO only.
                subprocess.run(['cc','-std=c11','-O1','-Wall','-Wextra','-Werror',
                    '-Wno-unused-parameter','-Wno-pointer-to-int-cast','-Wno-int-to-pointer-cast',
                    '-ffunction-sections','-fdata-sections',
                    *(['-Wl,-dead_strip'] if sys.platform=='darwin' else ['-Wl,--gc-sections']),
                    '-DHOST_TEST',*(['-DUSB_ROLE_HOST=1'] if host else []),
                    '-I'+str(dest),'-Iinclude','tests/usb_wait.c','drivers/usb_wait.c','core/usblog.c','core/usbhost_stats.c',
                    str(dest/'common/tusb_fifo.c'),str(dest/'tusb.c'),'-o',str(exe)],cwd=ROOT,check=True)
                cases=[0,1,2,3,4,7,12,13,14]+([5,6] if host else [8,9,10,11])
                for case in cases:
                    variants=range(4) if case==6 else range(2) if case in (0,3,4) else range(1)
                    for variant in variants:
                        for start in (0,0xfffffff0):
                            with self.subTest(host=host,case=case,start=start,variant=variant):
                                subprocess.run([str(exe),str(case),str(start),str(variant)],cwd=ROOT,check=True,timeout=5)
