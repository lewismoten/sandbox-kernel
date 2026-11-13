#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Build JS.ELF twice from scratch, from two copies of js/ and ../sdk at different paths, and
fail unless the stripped and unstripped ELFs are byte-identical.
Usage: repro.py"""
import hashlib, pathlib, shutil, subprocess, sys, tempfile

JS = pathlib.Path(__file__).resolve().parents[1]

def build(js):
    subprocess.run(['make', '-C', str(js), 'BUILD=build', 'build/JS.ELF'], check=True, capture_output=True)
    return {n: hashlib.sha256((js / 'build' / n).read_bytes()).hexdigest() for n in ('JS.ELF', 'JS.full.elf')}

def main():
    with tempfile.TemporaryDirectory() as t:
        t = pathlib.Path(t)
        r = []
        for name in ('one', 'two/deeper'):  # two clean copies at different paths, each built once
            (t / name).mkdir(parents=True)
            shutil.copytree(JS, t / name / 'js', ignore=shutil.ignore_patterns('build'))
            shutil.copytree(JS.parent / 'sdk', t / name / 'sdk')
            r.append(build(t / name / 'js'))
    ok = r[0] == r[1]
    for k in r[0]: print(f"{k}: {r[0][k][:16]} {r[1][k][:16]}")
    print('reproducible' if ok else 'NOT reproducible')
    return 0 if ok else 1

if __name__ == '__main__':
    sys.exit(main())
