# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class PowerHardware(unittest.TestCase):
    def test_firmware_transitions(self):
        with tempfile.TemporaryDirectory() as d:
            exe=str(Path(d)/'power')
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-O1','-fno-builtin','-DHOST_TEST','-D_FORTIFY_SOURCE=0',
                            '-Iinclude','core/power.c','drivers/power.c','core/util.c',
                            'tests/power_hw.c','-o',exe],cwd=ROOT,check=True)
            subprocess.run([exe],cwd=ROOT,check=True,timeout=10)
