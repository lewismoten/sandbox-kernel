# SPDX-License-Identifier: MIT
"""Sandbox JS (js/): the host runtime's fixtures, the ARM build, the size budget, and byte-identical
rebuilds. The ARM parts skip when the toolchain is absent."""
import os, pathlib, shutil, struct, subprocess, sys, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
ARM = os.environ.get('TOOLCHAIN', '/Applications/ArmGNUToolchain/15.3.rel2/arm-none-eabi/bin/arm-none-eabi-') + 'gcc'
ARM_AVAILABLE = shutil.which(ARM) is not None
BUILD = tempfile.mkdtemp(prefix='sandbox-js-')

def make(*args):
    return subprocess.run(['make', '-C', str(ROOT / 'js'), f'BUILD={BUILD}', *args], capture_output=True, text=True)

class SandboxJs(unittest.TestCase):
    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(BUILD, ignore_errors=True)

    def test_host_fixtures_and_contract(self):
        r = make('test')
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        c = subprocess.run([sys.executable, str(ROOT / 'js/tools/contract.py'), 'check'], capture_output=True, text=True)
        self.assertEqual(c.returncode, 0, c.stdout + c.stderr)

    @unittest.skipUnless(ARM_AVAILABLE, 'arm-none-eabi-gcc not found')
    def test_arm_build_obeys_loader_rules_and_size_budget(self):
        r = make('size')
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])
        d = pathlib.Path(BUILD, 'JS.ELF').read_bytes()
        etype, machine = struct.unpack_from('<HH', d, 16)
        entry, phoff = struct.unpack_from('<II', d, 24)
        phentsize, phnum = struct.unpack_from('<HH', d, 42)
        self.assertEqual((etype, machine, entry & 3), (2, 40, 0))
        loads = [struct.unpack_from('<8I', d, phoff + i * phentsize) for i in range(phnum)]
        loads = [l for l in loads if l[0] == 1]
        self.assertLessEqual(len(loads), 4)
        self.assertFalse(any(l[6] & 2 and l[6] & 1 for l in loads), 'W+X segment')
        self.assertLessEqual(len(d), 1 << 20)

    @unittest.skipUnless(ARM_AVAILABLE, 'arm-none-eabi-gcc not found')
    def test_two_builds_are_byte_identical(self):
        r = make('repro')
        self.assertEqual(r.returncode, 0, r.stdout[-2000:] + r.stderr[-2000:])

if __name__ == '__main__':
    unittest.main()
