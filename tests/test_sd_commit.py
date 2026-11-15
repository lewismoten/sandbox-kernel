# SPDX-License-Identifier: MIT
"""Host build of the real driver's cooperative busy loops over fake registers."""
import pathlib
import subprocess
import tempfile
import unittest
import hostlib


class SdCommit(unittest.TestCase):
    def test_controller_and_card_waits_yield_and_timeout(self):
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp)/'sd-commit'
            subprocess.run(['cc', '-std=c11', '-O1', '-DHOST_TEST', '-fno-builtin',
                            '-Wall', '-Wextra', '-Werror', '-I'+str(hostlib.ROOT/'include'),
                            str(hostlib.ROOT/'tests/sd_commit_host.c'),
                            str(hostlib.ROOT/'core/cache_commit.c'), '-o', str(out)], check=True)
            result = subprocess.run([str(out)], capture_output=True, text=True, timeout=15)
            self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
            print(result.stdout, end='')
