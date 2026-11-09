#!/usr/bin/env python3
"""Render the e-ink screens and the HDMI boot overlay and recovery console on the host to PNG (no board needed).
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
                str(ROOT / 'core/render.c'), str(ROOT / 'core/eink.c'), str(ROOT / 'core/font.c'), str(ROOT / 'core/logo_data.c'), str(ROOT / 'core/util.c'), str(ROOT / 'core/divide.c'), '-o', str(lib)], check=True)
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

# E-ink API (docs/eink.md): the title screen, an OS page, and the page with the kernel's error mark.
def eink_shot(name, error, setup):
    setup(); buf = (ctypes.c_uint8 * 4000)(); L.eink_compose(buf, b'5ca68d431228', b'reader', error, ctypes.c_uint32(3725))
    png(out / name, view(bytes(buf))); print(out / name)
def test_page():
    """A plain test bitmap: a frame, a corner ruler and a round face (not a character)."""
    b = bytearray(3904)
    def px(x, y):
        if 0 <= x < 250 and 0 <= y < 122: b[y * 32 + x // 8] |= 0x80 >> (x % 8)
    for x in range(250): px(x, 0); px(x, 121)
    for y in range(122): px(0, y); px(249, y)
    for i in range(0, 120, 10):
        for k in range(4 + (i % 20 == 0) * 4): px(2 + i + 8, 3 + k)  # ruler ticks along the top
    cx, cy = 125, 66
    for y in range(122):
        for x in range(250):
            d = (x - cx) ** 2 + (y - cy) ** 2
            if 42 ** 2 - 90 <= d <= 42 ** 2 + 90: px(x, y)                                      # the head outline
            if (x - 110) ** 2 + (y - 54) ** 2 <= 36 or (x - 140) ** 2 + (y - 54) ** 2 <= 36: px(x, y)   # eyes
            if 20 ** 2 - 40 <= (x - cx) ** 2 + (y - 74) ** 2 <= 20 ** 2 + 40 and y > 78: px(x, y)      # a smile
    return bytes(b)
def set_title():
    mark = bytearray(200)
    for y in range(40):
        for x in range(40):
            if (x - 20) ** 2 + (y - 20) ** 2 <= 18 ** 2 and (x - 20) ** 2 + (y - 20) ** 2 >= 13 ** 2 or (x - 20) ** 2 + (y - 20) ** 2 <= 36: mark[y * 5 + x // 8] |= 0x80 >> (x % 8)
    L.eink_title(b'TEST OS', 7, (ctypes.c_uint8 * 200).from_buffer(mark), b'v0.4 3a9f2c1', 12, ctypes.c_uint64(10**9))
def set_page():
    page = test_page(); L.eink_page((ctypes.c_uint8 * 3904).from_buffer_copy(page), 3904, ctypes.c_uint64(2 * 10**9))
shot('eink-kernel-ok.png', L.render_status, b'5ca68d431228', b'reader', b'none', ctypes.c_uint32(3725))
eink_shot('eink-title.png', b'none', set_title)
eink_shot('eink-page.png', b'none', set_page)
def page_then_error():
    set_page(); eink_shot_hidden = (ctypes.c_uint8 * 4000)(); L.eink_compose(eink_shot_hidden, b'x', b'x', b'SD READ FAIL', ctypes.c_uint32(1))  # the error wins once
    L.eink_page((ctypes.c_uint8 * 3904).from_buffer_copy(test_page()), 3904, ctypes.c_uint64(4 * 10**9))
eink_shot('eink-page-error.png', b'SD READ FAIL', page_then_error)
L.eink_reset()

# HDMI: the boot screen (bars, ramp, title panel) at both sizes, and the recovery console with log lines.
sys.path.insert(0, str(ROOT / 'tests'))
import hostlib

class Fb(ctypes.Structure):
    _fields_ = [(n, ctypes.c_uint32) for n in ('width', 'height', 'depth', 'pitch', 'bytes', 'pixel_order', 'bus', 'phys')]

def rgb_png(path, f, buf, scale=1):
    rows = []
    for y in range(f.height):
        row = bytearray()
        for x in range(f.width):
            v = int.from_bytes(buf[y * f.pitch + x * 4:y * f.pitch + x * 4 + 4], 'little')
            row += bytes(((v >> 16) & 255, (v >> 8) & 255, v & 255)) * scale
        rows += [b'\x00' + bytes(row)] * scale
    def chunk(t, d): c = struct.pack('>I', len(d)) + t + d; return c + struct.pack('>I', zlib.crc32(t + d))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', f.width * scale, f.height * scale, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(b''.join(rows))) + chunk(b'IEND', b''))
    print(path)

H = ctypes.CDLL(str(hostlib.build(pathlib.Path(tempfile.mkdtemp()) / 'h.dylib')))
for w, h, name in ((960, 540, 'hdmi-boot-960x540.png'), (640, 480, 'hdmi-boot-640x480.png')):
    f = Fb(w, h, 32, w * 4 + 64, 0, 0, 0, 0); buf = (ctypes.c_uint8 * (f.pitch * h))()
    H.fb_boot_screen(ctypes.byref(f), buf); rgb_png(out / name, f, bytes(buf))
for line in (b'sandbox-kernel 5ca68d431228 Zero W v1.1 RASPPI=1', b'FB ok 960x540 32 3904', b'ASSET mount FAT32 ok', b'INPUT /dev/input/event0 synthetic (USB KEY, AXIS)',
             b'OS autostart in 10 s: GMOS.ELF', b'OS reject /OS/GMOS.ELF bad header', b'OS none; recovery console (USB PUTF and RUN available)'):
    H.console_log(line)
f = Fb(960, 540, 32, 960 * 4 + 64, 0, 0, 0, 0); buf = (ctypes.c_uint8 * (f.pitch * 540))()
H.fb_console.restype = ctypes.c_bool; H.fb_console(ctypes.byref(f), buf); rgb_png(out / 'hdmi-recovery-960x540.png', f, bytes(buf))
