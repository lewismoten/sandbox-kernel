#!/usr/bin/env python3
# SPDX-License-Identifier: MIT; turns a JS file into a C source holding it as
# a NUL-terminated string (jsrt_prelude, jsrt_prelude_len) for JS.ELF.
import sys

src, out, name = sys.argv[1], sys.argv[2], sys.argv[3]
data = open(src, 'rb').read()
with open(out, 'w') as f:
    f.write(f'/* Generated from {src} by tools/embed_js.py. */\n#include <stdint.h>\n')
    f.write(f'const char {name}[] = {{\n')
    for i in range(0, len(data), 16):
        f.write(''.join(f'{b},' for b in data[i:i + 16]) + '\n')
    f.write(f'0}};\nconst uint32_t {name}_len = {len(data)}u;\n')
