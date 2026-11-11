# SPDX-License-Identifier: MIT
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]
class BcmAsync(unittest.TestCase):
    def test_lost_reply_abandoned(self):
        with tempfile.TemporaryDirectory() as d:
            exe=str(Path(d)/'bcm')
            subprocess.run(['cc','-std=gnu11','-Wall','-O1','-fno-builtin','-DHOST_TEST','-D_FORTIFY_SOURCE=0',
                            '-Iinclude','-Idrivers','tests/bcm_async.c','-o',exe],cwd=ROOT,check=True)
            subprocess.run([exe],cwd=ROOT,check=True,timeout=10)
