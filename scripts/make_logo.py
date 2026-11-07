#!/usr/bin/env python3
"""Draw the Game-Maker: Sandbox Edition logo (sandbox, sand castle, pail, shovel,
the subject of the Sandbox sting's last frame) as clean 1-bit art and write
core/logo_data.c. Own drawing, MIT. Usage: scripts/make_logo.py [PREVIEW.png [BADGE512.png]]
Needs Pillow. A round badge in two sizes from one drawing: LARGE 104x104, SMALL 40x40.
Optional second argument: a 512x512 PNG of the large badge."""
import pathlib, sys
from PIL import Image, ImageDraw
ROOT = pathlib.Path(__file__).resolve().parent.parent

def scene(S,ys=1.0):
    """Drawing in 200x76 units, rendered at factor S, 1-bit; returns Image mode '1' (0 = black)."""
    W, H = round(200 * S), round(76 * S * ys)
    big = 4  # draw 4x then threshold-downsample for even strokes
    im = Image.new('L', (W * big, H * big), 255); d = ImageDraw.Draw(im)
    k = kx = S * big; ky = kx * ys
    def P(*pts): return [(x * kx, y * ky) for x, y in pts]
    lw = max(big, round(1.4 * S * big))
    def poly(pts, fill=None, out=0): d.polygon(P(*pts), fill=fill, outline=None); d.line(P(*pts, pts[0]), fill=out, width=lw, joint='curve')
    def rect(x0, y0, x1, y1, fill=255, out=0): poly([(x0, y0), (x1, y0), (x1, y1), (x0, y1)], fill, out)
    def stipple(box, step, shade=0):
        x0, y0, x1, y1 = box; y = y0
        while y < y1:
            x = x0 + ((int(y / step) % 2) * step / 2)
            while x < x1: d.rectangle([x * kx, y * ky, x * kx + max(big * 0.8, S * big * 0.9), y * ky + max(big * 0.8, S * big * 0.9)], fill=shade); x += step
            y += step / 2
    # sandbox: sand surface, wooden frame
    poly([(14, 50), (186, 50), (198, 62), (2, 62)], 255); stipple((22, 53, 182, 61), 4)
    rect(0, 62, 199, 75); d.line(P((2, 68.5), (197, 68.5)), fill=255, width=lw)
    d.line(P((0, 62), (199, 62)), fill=0, width=lw * 2)
    # castle: keep, two side towers, crenels, tall tower, flag, gate
    rect(84, 24, 116, 52); rect(64, 32, 82, 52); rect(118, 34, 136, 52); rect(92, 8, 108, 24)
    for x in range(84, 116, 8): rect(x, 19, x + 4, 24, 0)
    for x in range(64, 82, 6): rect(x, 27, x + 3, 32, 0)
    for x in range(118, 136, 6): rect(x, 29, x + 3, 34, 0)
    for x in (92, 99, 105): rect(x, 4, x + 3, 8, 0)
    d.line(P((100, 4), (100, 0)), fill=0, width=lw); poly([(100, 0.5), (111, 2.5), (100, 5)], 0)
    poly([(94, 52), (94, 42), (100, 37), (106, 42), (106, 52)], 0)
    stipple((86, 26, 114, 36), 5); stipple((66, 36, 80, 50), 5); stipple((120, 38, 134, 50), 5)
    d.rectangle(P((96, 12), (98, 16))[0] + P((96, 12), (98, 16))[1], fill=0)
    # pail: tapered bucket, rim, handle, sand heap
    poly([(18, 32), (54, 32), (49, 52), (23, 52)], 255)
    for y in (36, 41, 46): d.line(P((19.6 + (y - 32) * .25, y), (52.4 - (y - 32) * .25, y)), fill=0, width=lw)
    d.line(P((18, 32), (54, 32)), fill=0, width=lw * 2)
    d.arc(P((20, 22), (52, 42))[0] + P((20, 22), (52, 42))[1], 180, 360, fill=0, width=lw)
    poly([(22, 31), (30, 24), (42, 24), (50, 31)], 255)
    # shovel: handle and blade, a pat of sand
    d.line(P((184, 36), (150, 54)), fill=0, width=lw * 2)
    poly([(146, 50), (160, 52), (156, 60), (140, 58)], 0)
    d.ellipse(P((178, 31), (190, 39))[0] + P((178, 31), (190, 39))[1], outline=0, width=lw)
    # grains of sand
    for gx, gy in [(150, 46), (156, 42), (162, 47), (168, 44), (140, 44)]: d.rectangle([gx * kx, gy * ky, gx * kx + kx, gy * ky + kx], fill=0)
    return im

def badge(N):
    """Square N x N badge: the scene inside a circle, sun, cloud and a checkered lawn."""
    big = 4; M = N * big
    im = Image.new('L', (M, M), 255); d = ImageDraw.Draw(im)
    sc = scene(N * 0.0047, 1.9)
    base = int(0.81 * M)
    im.paste(sc, ((M - sc.width) // 2, base - sc.height))
    # checkered lawn below the sandbox (in the sting, the sandbox sits on one)
    cell = max(2, round(N * 0.07)) * big // 1
    for gy in range(base, M, cell):
        for gx in range(0, M, cell):
            if ((gx // cell) + (gy // cell)) % 2 == 0: d.rectangle([gx, gy + 3 * big, gx + cell - 1, gy + cell - 1 + 3 * big], fill=0)
    d.rectangle([0, base, M, base + 2 * big], fill=255)
    # sun (disc with eight short rays) and a cloud
    cx, cy, r = 0.25 * M, 0.21 * M, 0.04 * M
    d.ellipse([cx - r, cy - r, cx + r, cy + r], fill=0)
    import math
    for i in range(8):
        a = i * math.pi / 4 + math.pi / 8
        d.line([cx + math.cos(a) * r * 1.4, cy + math.sin(a) * r * 1.4, cx + math.cos(a) * r * 1.9, cy + math.sin(a) * r * 1.9], fill=0, width=max(big, round(0.012 * M)))
    for ox, oy, rr in ((0.68, 0.19, 0.03), (0.73, 0.175, 0.04), (0.78, 0.195, 0.03)):
        d.ellipse([(ox - rr) * M, (oy - rr) * M, (ox + rr) * M, (oy + rr) * M], outline=0, fill=255, width=max(big, round(0.012 * M)))
    d.rectangle([0.60 * M, 0.20 * M, 0.86 * M, 0.235 * M], fill=255)
    d.line([0.65 * M, 0.2185 * M, 0.81 * M, 0.2185 * M], fill=0, width=max(big, round(0.012 * M)))
    # clip to the circle, then draw the ring
    mask = Image.new('L', (M, M), 0); ImageDraw.Draw(mask).ellipse([0, 0, M - 1, M - 1], fill=255)
    out = Image.new('L', (M, M), 255); out.paste(im, (0, 0), mask); d = ImageDraw.Draw(out)
    ring = max(2 * big, round(0.045 * M))
    d.ellipse([0, 0, M - 1, M - 1], outline=0, width=ring)
    return out.resize((N, N), Image.BOX).point(lambda v: 255 if v > 140 else 0).convert('1')

def pack(im):
    w, h = im.size; stride = (w + 7) // 8; rows = []
    for y in range(h):
        row = bytearray(stride)
        for x in range(w):
            if im.getpixel((x, y)) == 0: row[x // 8] |= 0x80 >> (x % 8)  # 1 = black
        rows.append(row)
    return w, h, stride, b''.join(rows)

out = ["/* SPDX-License-Identifier: MIT. Generated by scripts/make_logo.py; do not edit. */",
       '#include "logo.h"']
sheet = Image.new('1', (160, 108), 1); x0 = 0
for name, S in (('large', 104), ('small', 40)):
    im = badge(S); w, h, stride, data = pack(im)
    sheet.paste(im, (x0, 0)); x0 += w + 6
    out.append(f"static const uint8_t {name}_bits[]={{" + ','.join(f'0x{b:02x}' for b in data) + '};')
    out.append(f"const struct logo logo_{name}={{{w},{h},{stride},{name}_bits}};")
(ROOT / 'core/logo_data.c').write_text('\n'.join(out) + '\n')
if len(sys.argv) > 2: badge(104).resize((512, 512), Image.NEAREST).convert('1').save(sys.argv[2])
if len(sys.argv) > 1: sheet.resize((sheet.width * 4, sheet.height * 4), Image.NEAREST).save(sys.argv[1])
