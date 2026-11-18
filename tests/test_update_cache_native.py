# SPDX-License-Identifier: MIT
"""Standalone C11 real protocol/syscall/FAT COW sanitizer composition.
The cooperative worker uses libc ucontext on the host: ASan warns that
makecontext/swapcontext support is incomplete. Keep that warning visible; this
is supplementary native evidence, not an ARM stack or physical timing claim.
"""
from pathlib import Path
import hashlib
import os
import shlex
import subprocess
import tempfile
import unittest
import hostlib
import test_m1_user as user


class UpdateCacheNative(unittest.TestCase):
    def test_real_cow_native_and_sanitized(self):
        with tempfile.TemporaryDirectory(prefix='update-cache-') as tmp:
            work = Path(tmp)
            for sanitize in (False, True):
                flags = ['-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                         '-fno-omit-frame-pointer', '-fno-pie', '-no-pie'] if sanitize else []
                exe = work/('sanitized' if sanitize else 'native')
                cmd = ['cc', '-std=c11', '-O1', '-g', '-fno-builtin', '-DHOST_TEST',
                       '-D_FORTIFY_SOURCE=0', '-Wall', '-Wextra', '-Werror', *flags,
                       '-I'+str(hostlib.ROOT/'tests/stubs'), '-I'+str(hostlib.ROOT/'include'),
                       *[str(hostlib.ROOT/s) for s in hostlib.SOURCES],
                       str(hostlib.ROOT/'tests/update_cache_native.c'), '-o', str(exe)]
                print('BUILD', shlex.join(cmd), flush=True)
                subprocess.run(cmd, check=True, timeout=60)
                print('BINARY SHA256', hashlib.sha256(exe.read_bytes()).hexdigest(), flush=True)
                if sanitize:
                    imports = subprocess.check_output(['nm', '-u', str(exe)], text=True)
                    self.assertIn('__asan_', imports)
                    self.assertIn('__ubsan_', imports)
                    print('ACTUAL BINARY ASan+UBSan imports verified', flush=True)
                payload = work/'next.ski'
                payload.write_bytes(user.image.pack(b'B'*768, 'native-overlap'))
                for phase, mode in ((4, 'complete'), (5, 'complete'), (6, 'complete'), (4, 'error')):
                    with self.subTest(sanitize=sanitize, phase=phase, mode=mode):
                        disk, _ = user.boot_card(work, {'DIAG.ELF': user.program()})
                        run = [str(exe), str(disk), str(payload), str(phase), mode]
                        print('RUN', shlex.join(run), flush=True)
                        result = subprocess.run(run, capture_output=True, text=True, timeout=30,
                                                env={**os.environ,
                                                     'ASAN_OPTIONS': 'detect_leaks=1:halt_on_error=1',
                                                     'UBSAN_OPTIONS': 'halt_on_error=1:print_stacktrace=1'})
                        print(result.stdout, result.stderr, flush=True)
                        self.assertEqual(result.returncode, 0, result.stdout+result.stderr)
                        disk.unlink()


if __name__ == '__main__': unittest.main(verbosity=2)
