# SPDX-License-Identifier: MIT
"""Host build check for examples/: each one compiles and links for ARM with the SDK only, and its
ELF obeys the loader rules (ARM EXEC, ARM state entry, 4 loads at most, no W+X, 8 MiB image quota)."""
import pathlib, struct, subprocess, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]
EXAMPLES = {'hello': 'HELLO.ELF', 'clock': 'CLOCK.ELF'}
QUOTA = 8 << 20
FILE_MAX = 1 << 20

class ExampleBuild(unittest.TestCase):
    def check(self, name, elf):
        folder = ROOT / 'examples' / name
        subprocess.run(['make', '-C', str(folder), '-B'], check=True, capture_output=True)
        data = (folder / elf).read_bytes()
        self.assertLessEqual(len(data), FILE_MAX)
        self.assertEqual(data[:4], b'\x7fELF')
        etype, machine = struct.unpack_from('<HH', data, 16)
        entry, phoff = struct.unpack_from('<II', data, 24)
        phentsize, phnum = struct.unpack_from('<HH', data, 42)
        self.assertEqual((etype, machine), (2, 40))  # ET_EXEC, ARM
        self.assertEqual(entry & 3, 0)               # ARM state, aligned
        pages = 0; loads = 0
        for i in range(phnum):
            t, off, va, pa, fsz, msz, flags, al = struct.unpack_from('<8I', data, phoff + i * phentsize)
            if t != 1: continue
            loads += 1
            self.assertNotEqual(flags & 3, 3, 'segment is writable and executable')
            pages += ((va + msz + 4095) // 4096) - va // 4096
            self.assertGreaterEqual(va, 0x08000000); self.assertLess(va + msz, 0x10000000)
        self.assertLessEqual(loads, 4)
        self.assertLessEqual(pages * 4096, QUOTA, 'image quota')
        self.assertLess(len(data), 64 * 1024, 'an example should stay tiny')
    def test_hello(self): self.check('hello', 'HELLO.ELF')
    def test_clock(self): self.check('clock', 'CLOCK.ELF')
    def test_hello_is_short(self):
        self.assertLess(len((ROOT / 'examples/hello/main.c').read_text().splitlines()), 150)

if __name__ == '__main__': unittest.main()
