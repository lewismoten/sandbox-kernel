# SPDX-License-Identifier: MIT
"""The kernel idle screen and launcher (core/launcher.c): real drawing code on a fake 960x540
framebuffer with a fake directory listing and a fake run; keys choose and start. Set
LAUNCHER_PNG_DIR to save the rendered screens."""
import ctypes as C, os, pathlib, subprocess, tempfile, unittest
from PIL import Image
import hostlib
ROOT = hostlib.ROOT
UP, DOWN, ENTER, OK = 103, 108, 28, 352

class Fb(C.Structure):
    _fields_ = [(n, C.c_uint32) for n in ('width', 'height', 'depth', 'pitch', 'bytes', 'pixel_order', 'bus', 'phys')]
LIST = C.CFUNCTYPE(C.c_uint, C.c_void_p)
RUN = C.CFUNCTYPE(C.c_int, C.c_char_p)
class Ops(C.Structure): _fields_ = [('list', LIST), ('run', RUN)]

class Launcher(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = tempfile.TemporaryDirectory(); lib = pathlib.Path(cls.t.name) / 'l.dylib'
        subprocess.run(['cc', '-shared', '-fPIC', '-O1', '-fno-builtin', '-DHOST_TEST', '-D_FORTIFY_SOURCE=0', '-Wall', '-Wextra', '-Werror',
                        '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'tests/stubs'),
                        *[str(ROOT / f'core/{n}.c') for n in ('launcher', 'font', 'util', 'divide')], '-o', str(lib)], check=True)
        cls.L = C.CDLL(str(lib)); cls.L.launcher_name.restype = C.c_char_p; cls.L.launcher_title.restype = C.c_char_p
        cls.L.launcher_message.restype = C.c_char_p; cls.L.launcher_draw.restype = C.c_bool
    @classmethod
    def tearDownClass(cls): cls.t.cleanup()
    def open(self, programs, stopped=b'/OS/GMOS.ELF', run_result=0):
        self.listed = 0; self.ran = []
        def lst(buf):
            self.listed += 1
            for i, n in enumerate(programs): C.memmove(buf + i * 13, n + b'\0', len(n) + 1)
            return len(programs)
        def run(path): self.ran.append(path); return run_result
        self.ops = Ops(LIST(lst), RUN(run)); self.L.launcher_open(C.byref(self.ops), stopped)
    def key(self, k): return self.L.launcher_key(C.c_uint(k))
    def draw(self, w=960, h=540):
        f = Fb(w, h, 32, w * 4 + 64, 0, 0, 0, 0); buf = (C.c_uint8 * (f.pitch * h))()
        ok = self.L.launcher_draw(C.byref(f), buf); return ok, f, bytes(buf)
    def png(self, name):
        d = os.environ.get('LAUNCHER_PNG_DIR')
        if not d: return
        ok, f, b = self.draw(); im = Image.frombuffer('RGBX', (960, 540), b, 'raw', 'BGRX', f.pitch, 1)
        pathlib.Path(d).mkdir(parents=True, exist_ok=True); im.convert('RGB').save(pathlib.Path(d) / name)
    def lit(self, b, f, box):
        x0, y0, x1, y1 = box
        return sum(b[y * f.pitch + x * 4:y * f.pitch + x * 4 + 3] != b'\x20\x14\x10' for y in range(y0, y1) for x in range(x0, x1))

    def test_titles(self):
        self.open([b'GMOS.ELF']); self.assertEqual(self.L.launcher_title(), b'GAME-MAKER OS STOPPED')
        self.open([b'GMOS.ELF'], None); self.assertEqual(self.L.launcher_title(), b'PROGRAMS')
        self.open([b'JS.ELF'], b'/OS/JS.ELF'); self.assertEqual(self.L.launcher_title(), b'JS.ELF STOPPED')
    def test_draws_and_highlights(self):
        self.L.launcher_footer(b'KERNEL GEN 7 5CA68D431228')
        self.open([b'GMOS.ELF', b'JS.ELF', b'DIAG.ELF'])
        ok, f, b = self.draw(); self.assertTrue(ok)
        px = lambda x, y: int.from_bytes(b[y * f.pitch + x * 4:y * f.pitch + x * 4 + 4], 'little')
        self.assertEqual(px(2, 2), 0x101420)
        self.assertEqual(px(60, 120), 0x305090)       # first row is the highlighted one
        self.assertEqual(px(60, 160), 0x101420)       # second is not
        self.key(DOWN); b = self.draw()[2]
        self.assertEqual(int.from_bytes(b[160 * f.pitch + 60 * 4:][:4], 'little'), 0x305090)
        self.assertGreater(self.lit(b, f, (48, 466, 912, 506)), 100)   # footer text drawn
        self.png('launcher-list.png')
    def test_only_960x540(self):
        self.open([b'GMOS.ELF']); self.assertFalse(self.draw(800, 600)[0])
    def test_up_down_wrap_and_enter_runs_the_path(self):
        self.open([b'GMOS.ELF', b'JS.ELF', b'DIAG.ELF'])
        self.assertEqual(self.L.launcher_selected(), 0)
        self.assertTrue(self.key(DOWN)); self.assertEqual(self.L.launcher_selected(), 1)
        self.key(DOWN); self.key(DOWN); self.assertEqual(self.L.launcher_selected(), 0)   # wraps
        self.key(UP); self.assertEqual(self.L.launcher_selected(), 2)
        self.key(UP); self.assertFalse(self.key(5)); self.assertEqual(self.ran, [])
        self.assertFalse(self.key(OK))                  # a start needs no redraw
        self.assertEqual(self.ran, [b'/OS/JS.ELF']); self.assertFalse(self.L.launcher_active())
        self.assertFalse(self.key(ENTER)); self.assertEqual(len(self.ran), 1)  # closed: keys ignored
    def test_failed_start_stays_and_says_so(self):
        self.open([b'GMOS.ELF'], run_result=-8)
        self.assertTrue(self.key(ENTER)); self.assertTrue(self.L.launcher_active()); self.assertNotEqual(self.L.launcher_message(), b'')
        self.png('launcher-failed.png')
    def test_empty_list_message_and_enter_rescans(self):
        self.open([]); self.assertEqual(self.L.launcher_count(), 0)
        self.assertEqual(self.listed, 1); self.png('launcher-empty.png')
        ok, f, b = self.draw(); self.assertGreater(self.lit(b, f, (48, 150, 912, 180)), 100)   # the message line
        self.assertTrue(self.key(ENTER)); self.assertEqual(self.listed, 2); self.assertEqual(self.ran, [])
        self.assertFalse(self.key(DOWN))
    def test_scrolls_past_eight(self):
        self.open([b'P%d.ELF' % i for i in range(12)])
        for _ in range(11): self.key(DOWN)
        self.assertEqual(self.L.launcher_selected(), 11)
        ok, f, b = self.draw(); self.assertTrue(ok); self.png('launcher-scrolled.png')
        self.assertEqual(int.from_bytes(b[(116 + 7 * 40) * f.pitch + 60 * 4:][:4], 'little'), 0x305090)  # selection on the last row
if __name__ == '__main__': unittest.main()
