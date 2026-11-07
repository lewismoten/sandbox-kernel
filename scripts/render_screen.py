#!/usr/bin/env python3
"""Render the e-ink status screens on the host to PNG (no board needed).
Usage: scripts/render_screen.py OUTDIR [--rotate 0|1]
Compiles core/render.c as the board does and draws the landscape view a
person sees, undoing the panel's native 122x250 mapping."""
import ctypes, subprocess, sys, tempfile, zlib, struct, pathlib
ROOT = pathlib.Path(__file__).resolve().parent.parent
out = pathlib.Path(sys.argv[1]); out.mkdir(parents=True, exist_ok=True)
rot = int(sys.argv[sys.argv.index('--rotate') + 1]) if '--rotate' in sys.argv else 1
lib = pathlib.Path(tempfile.mkdtemp()) / 'r.dylib'
subprocess.run(['cc', '-shared', '-fPIC', '-O1', '-fno-builtin', '-DHOST_TEST', f'-DPANEL_ROTATE_180={rot}',
                '-D_FORTIFY_SOURCE=0', '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'tests/stubs'),
                str(ROOT / 'core/render.c'), str(ROOT / 'core/logo_data.c'), str(ROOT / 'core/util.c'), str(ROOT / 'core/divide.c'), '-o', str(lib)], check=True)
L = ctypes.CDLL(str(lib))

def png(path, rows, scale=3):
    h, w = len(rows), len(rows[0])
    raw = b''.join(b'\x00' + bytes(v for px in r for v in [px] * scale) for r in rows for _ in range(scale))
    def chunk(t, d): c = struct.pack('>I', len(d)) + t + d; return c + struct.pack('>I', zlib.crc32(t + d))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', w * scale, h * scale, 8, 0, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(raw)) + chunk(b'IEND', b''))

def view(plane):
    rows = []
    for ly in range(122):
        row = []
        for lx in range(250):
            px, py = (121 - ly, lx) if rot else (ly, 249 - lx)
            row.append(255 if plane[py * 16 + px // 8] & (0x80 >> (px % 8)) else 0)
        rows.append(row)
    return rows

def shot(name, fn, *args):
    buf = (ctypes.c_uint8 * 4000)(); fn(buf, *args); png(out / name, view(bytes(buf))); print(out / name)

shot('starting.png', L.render_starting)
for name, usb, err, sec in [('error-sd.png', b'USB POWER MBOX TIMEOUT', b'SD ACMD41 BUSY', 7),
                            ('ok.png', b'enumerated', b'none', 3725),
                            ('long.png', b'TUSB INIT', b'SD READ BPB FILE OVERLAP DIR CLUSTER2', 90)]:
    shot(name, L.render_status, b'5ca68d431228', usb, err, ctypes.c_uint32(sec))
