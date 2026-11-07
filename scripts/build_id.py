#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Content ID from original build inputs, without reading repository metadata."""
import hashlib, json, pathlib, sys
root=pathlib.Path(__file__).resolve().parents[1]
if sys.argv[1:] == ['--hash']:
    h=hashlib.sha256()
    files=[root/'Makefile',root/'scripts/fetch.py',root/'scripts/firmware-lock.json']
    for folder in ['arch','core','drivers','include']: files.extend(p for p in (root/folder).rglob('*') if p.is_file())
    for p in sorted(files): h.update(str(p.relative_to(root)).encode()+b'\0'+p.read_bytes()+b'\0')
    print(h.hexdigest()[:12])
else:
    value=sys.argv[1]
    if not value or len(value)>31 or any(ord(c)<32 or ord(c)>126 for c in value): raise ValueError('Build ID must be 1–31 printable ASCII bytes')
    dest=root/'build/build_id.h';text='#define BUILD_ID '+json.dumps(value)+'\n'
    if not dest.exists() or dest.read_text()!=text: dest.write_text(text)
