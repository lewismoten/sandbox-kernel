#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Scoped native ASan/UBSan runner; no Python ctypes/ASan preloading required."""
import os
import pathlib
import subprocess
import sys
import tempfile
from typing import Any
import fatgen
import fatlfn
import hostlib


def main():
    with tempfile.TemporaryDirectory() as folder:
        work = pathlib.Path(folder)
        executable = work/'lfn-asan'
        subprocess.run(['cc', '-O1', '-g', '-fno-builtin', '-DHOST_TEST', '-D_FORTIFY_SOURCE=0',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        '-Wall', '-Wextra', '-Werror', '-I'+str(hostlib.ROOT/'include'),
                        '-I'+str(hostlib.ROOT/'tests/stubs'),
                        *[str(hostlib.ROOT/p) for p in hostlib.SOURCES],
                        str(hostlib.ROOT/'tests/lfn_sanitize.c'), '-o', str(executable)], check=True)
        tree: dict[str, Any] = {f'N{i:07}': bytes([i])*100 for i in range(40)}
        tree['GAMES'] = {'MYGAME~1': {'FILE.GAM': b'game bytes'*700}}
        names = {'/N0000000': 'A'*255, '/N0000001': 'B'*254,
                 '/N0000002': 'é'*128, '/N0000003': 'é'*127,
                 '/N0000004': 'X'*13, '/N0000005': 'Y'*26,
                 '/N0000006': 'Café Σς 𐐀 🎮.gam',
                 '/GAMES/MYGAME~1': 'My game directory',
                 '/GAMES/MYGAME~1/FILE.GAM': 'Long game filename.gam'}
        card = work/'card.img'
        v = fatlfn.build(card, tree, names, fragment=True)
        env = dict(os.environ, ASAN_OPTIONS=('detect_leaks=0' if sys.platform == 'darwin' else 'detect_leaks=1') + ':halt_on_error=1',
                   UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
        subprocess.run([str(executable), str(card)], env=env, check=True)
        # Each field-corruption image is scanned by the instrumented real reader
        # and dispatcher, with its surviving aliases opened and payloads read.
        short = v.entry_offset('/GAMES/MYGAME~1/FILE.GAM')
        with open(card, 'rb') as f:
            f.seek(short-64); original = f.read(64)
        for offset, value in [(0,b'\x55'), (0,b'\x02'), (13,b'\xff'), (12,b'\x01'),
                              (26,b'\x01'), (32,b'\xe5'), (33,b'\x00\xd8')]:
            fatgen.patch(card, short-64, original)
            fatgen.patch(card, short-64+offset, value)
            subprocess.run([str(executable), str(card)], env=env, check=True)
        print('Native sanitizer images: 1 valid + 7 malformed variants OK')

if __name__ == '__main__': main()
