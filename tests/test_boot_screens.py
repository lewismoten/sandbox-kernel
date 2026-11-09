# SPDX-License-Identifier: MIT
"""The boot screens: e-ink starting/status against the art's template plus kernel-font text,
and the HDMI overlay and recovery console against the art's own pixels (art/boot/)."""
import ctypes as C, pathlib, re, subprocess, tempfile, unittest
from PIL import Image
import hostlib
ROOT = hostlib.ROOT
ART = ROOT / 'art/boot'

def pbm(name):
    head, dims, data = (ART / name).read_bytes().split(b'\n', 2)
    w, h = map(int, dims.split()); stride = (w + 7) // 8
    return {(x, y) for y in range(h) for x in range(w) if data[y * stride + x // 8] & (0x80 >> (x % 8))}

def font():
    src = (ROOT / 'core/font.c').read_text()
    alpha = re.search(r'alphabet\[\]="(.*)";', src).group(1)
    rows = [int(n, 8) for n in re.findall(r'\b(0[0-7]{2})\b', src)]
    return {c: rows[i * 7:i * 7 + 7] for i, c in enumerate(alpha)}

def text(x, y, s):
    """Pixels of s in the kernel font at 2x: 12 px advance, 14 px space."""
    f, out = font(), set()
    for ch in s:
        if ch != ' ':
            for r, bits in enumerate(f[ch]):
                for c in range(5):
                    if bits >> (4 - c) & 1: out |= {(x + c * 2 + i, y + r * 2 + j) for i in (0, 1) for j in (0, 1)}
        x += 14 if ch == ' ' else 12
    return out

class Fb(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('width', 'height', 'depth', 'pitch', 'bytes', 'pixel_order', 'bus', 'phys')]

class Eink(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = tempfile.TemporaryDirectory(); lib = pathlib.Path(cls.t.name) / 'r.dylib'
        subprocess.run(['cc', '-shared', '-fPIC', '-O1', '-fno-builtin', '-DHOST_TEST', '-DPANEL_ROTATE_180=1', '-D_FORTIFY_SOURCE=0',
                        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'tests/stubs'), *[str(ROOT / f'core/{n}.c') for n in ('render', 'font', 'logo_data', 'util', 'divide')],
                        '-o', str(lib)], check=True)
        cls.L = C.CDLL(str(lib))
    @classmethod
    def tearDownClass(cls): cls.t.cleanup()
    def view(self, fn, *args):
        buf = (C.c_uint8 * 4000)(); fn(buf, *args); plane = bytes(buf)
        return {(lx, ly) for ly in range(122) for lx in range(250) if not plane[lx * 16 + (121 - ly) // 8] & (0x80 >> ((121 - ly) % 8))}
    def test_starting(self):
        want = pbm('eink-starting-template-250x122.pbm') | text(112, 24, 'SANDBOX') | text(112, 42, 'KERNEL') | text(112, 77, 'STARTING...')
        self.assertEqual(self.view(self.L.render_starting), want)
    def test_status(self):
        want = (pbm('eink-status-template-250x122.pbm') | text(54, 6, 'SANDBOX KERNEL') | text(54, 24, '5CA68D431228') | text(4, 48, 'UP:') | text(54, 48, '1H 2M')
                | text(4, 70, 'USB:') | text(54, 70, 'ENUMERATED') | text(4, 92, 'ERR:') | text(54, 92, 'NONE'))
        self.assertEqual(self.view(self.L.render_status, b'5CA68D431228', b'enumerated', b'none', C.c_uint32(3725)), want)
    def test_long_values_clip_at_16_characters(self):
        got = self.view(self.L.render_status, b'5CA68D431228', b'x', b'SD READ BPB FILE OVERLAP DIR', C.c_uint32(1))
        self.assertEqual(got & {(x, y) for x in range(54, 250) for y in range(92, 106)}, text(54, 92, 'SD READ BPB FILE'))
    def test_no_game_maker_in_the_kernel(self):
        for p in ('core/render.c', 'core/logo_data.c', 'core/fb_art.c'):
            self.assertNotRegex((ROOT / p).read_text().lower(), r'game-maker|castle|gamemaker')

def art_rgb(name, box):
    im = Image.open(ART / name).convert('RGB').crop(box); w, h = im.size
    return [[(lambda p: (255, 255, 255) if p == (0xf8, 0xfc, 0xf8) else p)(im.getpixel((x, y))) for x in range(w)] for y in range(h)]

class Hdmi(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = tempfile.TemporaryDirectory(); cls.H = C.CDLL(str(hostlib.build(pathlib.Path(cls.t.name) / 'h.dylib')))
    @classmethod
    def tearDownClass(cls): cls.t.cleanup()
    def shot(self, w, h, order=0, fn='fb_boot_screen'):
        f = Fb(w, h, 32, w * 4 + 64, 0, order, 0, 0); buf = (C.c_uint8 * (f.pitch * h))(*([0xaa] * (f.pitch * h)))
        getattr(self.H, fn)(C.byref(f), buf); b = bytes(buf)
        px = lambda x, y: int.from_bytes(b[y * f.pitch + x * 4:y * f.pitch + x * 4 + 4], 'little')
        return f, b, px
    def check_panel(self, w, h, x0, y0):
        f, b, px = self.shot(w, h)
        want = art_rgb('hdmi-overlay-960x540.png', (298, 312, 662, 408))
        for y in range(96):
            for x in range(364):
                r, g, bl = want[y][x]; self.assertEqual(px(x0 + x, y0 + y), r << 16 | g << 8 | bl, (x, y))
        ref = self.shot(w, h, fn='fb_pattern')[2]   # outside the panel: the plain bars and ramp
        for x, y in ((10, 10), (w // 2, 50), (w - 10, h - 10), (50, h - 20), (x0 - 1, y0), (x0 + 364, y0 + 95), (x0, y0 + 96)):
            self.assertEqual(px(x, y), ref(x, y), (x, y))
        for i in range(h): self.assertTrue(all(c == 0xaa for c in b[i * f.pitch + w * 4:(i + 1) * f.pitch]), 'pitch padding written')
        return px
    def test_overlay_960(self): self.check_panel(960, 540, 298, 312)
    def test_overlay_640(self): self.check_panel(640, 480, 138, 272)
    def test_ramp_stays_neutral(self):
        _, _, px = self.shot(960, 540)
        for k in range(16):
            v = px(k * 60 + 30, 520); self.assertEqual((v >> 16, (v >> 8) & 255, v & 255), (k * 17,) * 3)
    def test_rgb_order_swaps_panel_colors(self):
        _, _, bgr = self.shot(960, 540); _, _, rgb = self.shot(960, 540, order=1)
        mark = [(x, y) for y in range(312, 408) for x in range(298, 662) if bgr(x, y) not in (0, 0xffffff) and bgr(x, y) != rgb(x, y)]
        self.assertTrue(mark)
        for x, y in mark[:50]:
            v = bgr(x, y); self.assertEqual(rgb(x, y), (v & 255) << 16 | (v & 0xff00) | v >> 16)
    def test_other_sizes_left_alone(self):
        f, b, px = self.shot(800, 600); ref = self.shot(800, 600, fn='fb_pattern')[1]; self.assertEqual(b, ref)
        self.assertFalse(self.H.fb_console.__call__(C.byref(Fb(800, 600, 32, 3200, 0, 0, 0, 0)), (C.c_uint8 * 8)()))
    def test_recovery_console(self):
        H = self.H
        for s in (b'first', b'x' * 130) + tuple(b'line %d' % i for i in range(12)): H.console_log(s)
        f, b, px = self.shot(960, 540, fn='fb_console')
        hdr = art_rgb('hdmi-recovery-console-960x540.png', (96, 44, 520, 116))
        self.assertTrue(all(px(96 + x, 44 + y) == hdr[y][x][0] << 16 | hdr[y][x][1] << 8 | hdr[y][x][2] for y in range(72) for x in range(424)))
        self.assertEqual(px(0, 0), 0x101420); self.assertEqual(px(500, 24), 0x7888a0); self.assertEqual(px(500, 300), 0)
        # twelve rows show and nothing is drawn past them or past column 60 (x=840)
        def row_on(r): return sum(px(120 + c, 151 + r * 24 + j) == 0xffffff for c in range(720) for j in range(14))
        self.assertTrue(all(row_on(r) > 0 for r in range(12))); self.assertEqual(row_on(12), 0)
        self.assertEqual({0, 0xffffff}, {px(x, y) for x in range(120, 840) for y in range(151, 439)})
        self.assertEqual(px(842, 200), 0)

if __name__ == '__main__': unittest.main()
