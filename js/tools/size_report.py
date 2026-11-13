#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Size report for a program ELF: file bytes, each PT_LOAD, and the loaded image against the
two limits that matter: the 1 MiB file ceiling (the update transport and v1 target) and the
kernel's 8 MiB image quota (U_IMAGE_QUOTA, include/uspace.h; the span from the image base in
whole pages). Fails if the file is over 1 MiB or the image is within 64 KiB of the quota.
Usage: size_report.py <ELF> [--json]"""
import json, struct, sys

FILE_MAX = 1 << 20
QUOTA = 8 << 20
MARGIN = 64 << 10
BASE = 0x08000000  # sdk/lib/sandbox.ld


def report(path):
    d = open(path, 'rb').read()
    if d[:4] != b'\x7fELF': raise SystemExit(f'{path}: not an ELF')
    phoff, = struct.unpack_from('<I', d, 28)
    phentsize, phnum = struct.unpack_from('<HH', d, 42)
    loads, end = [], BASE
    for i in range(phnum):
        t, off, va, pa, fsz, msz, flags, al = struct.unpack_from('<8I', d, phoff + i * phentsize)
        if t != 1: continue
        loads.append({'vaddr': va, 'file': fsz, 'mem': msz, 'flags': ('R' if flags & 4 else '-') + ('W' if flags & 2 else '-') + ('X' if flags & 1 else '-')})
        end = max(end, va + msz)
    image = -(-(end - BASE) // 4096) * 4096
    return {'file_bytes': len(d), 'loads': loads, 'image_bytes': image,
            'file_ok': len(d) <= FILE_MAX, 'quota_ok': image <= QUOTA - MARGIN}


def main(argv):
    r = report(argv[0])
    if '--json' in argv: print(json.dumps(r))
    else:
        print(f"{argv[0]}: {r['file_bytes']} bytes ({r['file_bytes'] * 100 / FILE_MAX:.1f}% of the 1 MiB ceiling, {FILE_MAX - r['file_bytes']} spare): {'ok' if r['file_ok'] else 'TOO BIG'}")
        for l in r['loads']:
            print(f"  PT_LOAD {l['flags']} vaddr 0x{l['vaddr']:08x}  file {l['file']:>8}  memory {l['mem']:>8}")
        print(f"loaded image {r['image_bytes']} bytes ({r['image_bytes'] / 1048576:.2f} MiB of {QUOTA >> 20} MiB, headroom {(QUOTA - r['image_bytes']) // 1024} KiB, margin {MARGIN >> 10} KiB): {'ok' if r['quota_ok'] else 'TOO BIG'}")
    return 0 if r['file_ok'] and r['quota_ok'] else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
