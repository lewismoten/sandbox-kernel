#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Reproduce the bounded DWC2/OS_NONE port from verified pinned inputs, offline."""
from pathlib import Path
import hashlib, json, shutil, subprocess, sys
ROOT = Path(__file__).resolve().parents[1]

def generate(dest):
    source = ROOT / 'third_party/tinyusb'
    manifest = json.loads((source / 'SHA256.json').read_text())
    if not manifest['pin'].startswith('TinyUSB 0.21.0 '):
        raise ValueError('Expected pinned TinyUSB 0.21.0')
    for name, digest in manifest['files'].items():
        if hashlib.sha256((source / name).read_bytes()).hexdigest() != digest:
            raise ValueError('Unexpected TinyUSB source: ' + name)
    dest = Path(dest)
    shutil.rmtree(dest, ignore_errors=True)
    shutil.copytree(source / 'src', dest)
    subprocess.run(['patch', '-s', '-p0', '-i', str(ROOT / 'patches/tinyusb-0.21-bounded.patch')], cwd=dest, check=True)
    (dest / '.stamp').touch()

if __name__ == '__main__':
    generate(sys.argv[1] if len(sys.argv) > 1 else ROOT / 'build/tinyusb-bounded')
