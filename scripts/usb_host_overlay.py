#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Apply local host-only changes to pinned TinyUSB; never edit fetched inputs."""
from pathlib import Path
import hashlib, shutil, subprocess, sys
ROOT = Path(__file__).resolve().parents[1]
# Fail closed on an upstream version change (including a locally edited dependency).
HASHES = {
    "usbh.c": "2b537fc20eb9f6757d53c59896290a4942daa1e4ce3ed892220a13596beb6ab8",
    "hid_host.c": "83856a19edbec34129fc1a7477c371fdced2108e6f9ebf77247974455bb9f2d6",
    "hub.c": "36b4fdf26f819a7882e0d9e76c5189ae3819ca5cb600ad902e27b5aa4a2f74cd",
}
def generate(dest):
    dest = Path(dest); dest.mkdir(parents=True, exist_ok=True)
    for name, digest in HASHES.items():
        source = ROOT / 'third_party/tinyusb/src' / ('class/hid' if name == 'hid_host.c' else 'host') / name
        if hashlib.sha256(source.read_bytes()).hexdigest() != digest:
            raise ValueError('Unexpected TinyUSB 0.21 source: ' + name)
        shutil.copyfile(source, dest / name)
    subprocess.run(['patch', '-s', '-p0', '-i', str(ROOT / 'patches/tinyusb-0.21-host.patch')], cwd=dest, check=True)
    (dest / '.stamp').touch()
if __name__ == '__main__':
    generate(sys.argv[1] if len(sys.argv)>1 else ROOT / 'build/tinyusb-host')
