"""Isolated production HID parser: strict C11 native and ASan+UBSan binaries.
No USB wiring, descriptor capture or physical device acceptance.
"""
from pathlib import Path
import hashlib
import shlex
import subprocess
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[1]
CASES = ('huge', 'huge-zero', 'bits', 'id-positions', 'zero-min', 'zero-max',
         'fields', 'ids', 'unsupported', 'active-limit')

class HidRepTests(unittest.TestCase):
    def run_binary(self, sanitize):
        with tempfile.TemporaryDirectory(prefix='hidrep-') as d:
            exe = str(Path(d) / 'hidrep')
            flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                     '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'] if sanitize else []
            cmd = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                   '-O1', '-g', *flags, '-Iinclude', 'core/hidrep.c', 'tests/hidrep.c', '-o', exe]
            print('BUILD', shlex.join(cmd), flush=True)
            subprocess.run(cmd, cwd=ROOT, check=True, timeout=30)
            print('BINARY SHA256', hashlib.sha256(Path(exe).read_bytes()).hexdigest(), flush=True)
            if sanitize:
                symbols = subprocess.check_output(['nm', '-u', exe], text=True)
                self.assertIn('__asan_', symbols)
                self.assertIn('__ubsan_', symbols)
                print('ACTUAL BINARY sanitizer imports:', symbols, flush=True)
            for case in ('valid', *CASES):
                with self.subTest(sanitize=sanitize, case=case):
                    start = time.monotonic()
                    args = [] if case == 'valid' else [case]
                    result = subprocess.run([exe, *args], cwd=ROOT, capture_output=True,
                                            text=True, timeout=5)
                    elapsed = time.monotonic()-start
                    print(f'RUN {case}: exit={result.returncode} seconds={elapsed:.6f}',
                          result.stdout, result.stderr, flush=True)
                    self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                    self.assertLess(elapsed, 1.0)
                    self.assertEqual(result.stderr, '')

    def test_mouse_system_consumer(self): self.run_binary(False)
    def test_sanitized_production_parser(self): self.run_binary(True)

if __name__ == '__main__': unittest.main(verbosity=2)
