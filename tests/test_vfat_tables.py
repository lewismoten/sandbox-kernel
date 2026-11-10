# SPDX-License-Identifier: MIT
"""Independent per-scalar oracle for the generated, pinned simple-fold table."""
import ctypes as C
import importlib.util
import pathlib
import subprocess
import tempfile
import unittest
import hostlib

class UnicodeTable(unittest.TestCase):
    def test_every_scalar_matches_pinned_unicode_simple_fold_and_generator(self):
        root = hostlib.ROOT
        spec = importlib.util.spec_from_file_location('unicode_fold_generator', root/'scripts/unicode_fold.py')
        assert spec is not None and spec.loader is not None
        generator = importlib.util.module_from_spec(spec); spec.loader.exec_module(generator)
        self.assertEqual(generator.generate(), (root/'include/vfat_fold.h').read_text())
        expected = {}
        for raw in (root/'scripts/data/CaseFolding-16.0.0.txt').read_text().splitlines():
            data = raw.partition('#')[0].split(';')
            if len(data) >= 3 and data[1].strip() in ('C', 'S'):
                expected[int(data[0], 16)] = int(data[2], 16)
        self.assertEqual(len(expected), 1484)
        with tempfile.TemporaryDirectory() as folder:
            work = pathlib.Path(folder)
            source = work/'fold.c'
            source.write_text('#include <stdint.h>\n#include "vfat_fold.h"\nuint32_t fold(uint32_t c);\nuint32_t fold(uint32_t c){return vfat_fold(c);}\n')
            subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I'+str(root/'include'), str(source), '-o', str(work/'fold.so')], check=True)
            library = C.CDLL(str(work/'fold.so'))
            library.fold.argtypes = [C.c_uint32]; library.fold.restype = C.c_uint32
            for scalar in range(0x110000):
                self.assertEqual(library.fold(scalar), expected.get(scalar, scalar), hex(scalar))
