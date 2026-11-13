#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 6 tests: examples/TINYOS booted through AUTORUN on the host: the home screen (framebuffer CRC fixture), key navigation,
a program inside it, a setting saved and read back after a restart, the chime (no underruns), the e-ink clock, back home, and a
clean exit with no handle left open. Usage: os_run.py <jsrun>   (--update rewrites tests/os_fixtures.json)"""
import json, pathlib, shutil, struct, subprocess, sys, tempfile, unittest, zlib

JS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(JS / 'tools'))
import app_check
FIX = JS / 'tests/os_fixtures.json'
RUN = None
UPDATE = False
K = {'up': 103, 'down': 108, 'left': 105, 'right': 106, 'enter': 28, 'esc': 1, 'q': 16}


def card(others=True):
    d = pathlib.Path(tempfile.mkdtemp())
    shutil.copytree(JS / 'examples/TINYOS', d / 'OS/APPS/TINYOS')
    (d / 'OS/AUTORUN.TXT').write_text('JS.ELF\nAPPS/TINYOS\n')
    if not others:
        return d
    shutil.copytree(JS / 'apps/HELLO', d / 'OS/APPS/HELLO')   # a real app to list and launch
    (d / 'OS/APPS/BROKEN').mkdir()   # an app that cannot run: listed, with the reason
    (d / 'OS/APPS/BROKEN/APP.JSON').write_text('{"name": "Broken one", "api": 99}')
    (d / 'OS/AUTORUN.TXT').write_text('JS.ELF\nAPPS/TINYOS\n')
    return d


def boot(d, keys=(), ms=2000, extra=()):
    """Runs TINYOS from AUTORUN on card d. keys: names, 100 ms apart from 300 ms. Returns (rc, log lines, shot crc, eink pages, audio bytes)."""
    k = []
    for i, name in enumerate(keys):
        k += ['%d:%d:1' % (300 + i * 100, K[name]), '%d:%d:0' % (320 + i * 100, K[name])]
    t = pathlib.Path(tempfile.mkdtemp())
    try:
        a = [str(RUN), str(d), 'x', '--autorun', '--vclock', '--ms', str(ms), '--shot', str(t / 's'), '--eink-dump', str(t / 'e'),
             '--audio-dump', str(t / 'a')] + (['--keys', ','.join(k)] if k else []) + list(extra)
        p = subprocess.run(a, capture_output=True, text=True, timeout=120)
        lines = [l[3:] if l.startswith('JS ') else l for l in (p.stdout + p.stderr).splitlines()]
        raw = (t / 's').read_bytes() if (t / 's').exists() else b''
        crc = zlib.crc32(raw[784:]) if raw else None
        e = (t / 'e').read_bytes() if (t / 'e').exists() else b''
        pages = [e[i + 8:i + 8 + 3904] for i in range(0, len(e), 8 + 3904) if e[i] == 0]   # kind 0 is a page (1 clear, 2 title)
        au = (t / 'a').read_bytes() if (t / 'a').exists() else b''
        return p.returncode, lines, crc, pages, au
    finally:
        shutil.rmtree(t, ignore_errors=True)


def fixture(name, value):
    fx = json.loads(FIX.read_text()) if FIX.exists() else {}
    if UPDATE:
        fx[name] = value
        FIX.write_text(json.dumps(fx, indent=1, sort_keys=True) + '\n')
    return fx.get(name)


class Tiny(unittest.TestCase):
    def setUp(self):
        self.d = card()
        self.addCleanup(shutil.rmtree, self.d, True)

    def clean(self, lines, leaks=True):
        # a run cut off by --ms (no quit key) still holds the panel, as a real power-off would; only a quit must leave none
        bad = [l for l in lines if 'Uncaught' in l or 'Could not' in l or leaks and 'left ' in l and 'open' in l]
        self.assertEqual(bad, [], lines)

    def test_app_check_passes_on_the_card(self):
        only = card(False)
        self.addCleanup(shutil.rmtree, only, True)
        self.assertEqual(app_check.card(str(only), None), 0)

    def test_boots_through_autorun_and_shows_home(self):
        rc, lines, crc, pages, au = boot(self.d, ms=1000)
        self.assertIn('autorun OS/APPS/TINYOS', lines)
        self.assertIn('TINYOS ready, 3 programs, 2 apps', lines)
        self.assertEqual(fixture('home', crc), crc, 'home screen framebuffer changed (--update to accept)')
        self.assertNotIn('picker', ' '.join(lines[-3:]))

    def test_navigate_open_a_program_and_go_home(self):
        _, _, home, _, _ = boot(self.d, ms=1000)
        for keys in (['down', 'enter', 'esc', 'up'],            # Files
                     ['enter', 'esc'],                          # About
                     ['down', 'down', 'enter', 'down', 'esc', 'up', 'up'],   # Settings
                     ['down', 'enter', 'down', 'enter', 'esc', 'esc', 'up']):   # Files: read a file, close it, and back out
            rc, lines, crc, _, _ = boot(self.d, keys, ms=1500)
            self.clean(lines, False)
            self.assertEqual(crc, home, keys)
        _, _, inside, _, _ = boot(self.d, ['down', 'enter'], ms=1500)
        self.assertNotEqual(inside, home)
        self.assertEqual(fixture('files', inside), inside)

    def test_home_lists_the_real_apps_and_launches_hello_and_comes_back(self):
        rc, lines, crc, _, _ = boot(self.d, ms=1000)
        self.assertIn('TINYOS ready, 3 programs, 2 apps', lines)
        self.assertEqual(fixture('home', crc), crc, 'home screen framebuffer changed (--update to accept)')
        rc, lines, back, _, _ = boot(self.d, ['down'] * 4 + ['enter', 'esc'], ms=3000)   # Files, ... Broken one, HELLO: Enter, then Esc inside HELLO
        self.assertIn('hello OS ready', lines)
        self.assertIn('exit HELLO to TINYOS, depth 0', lines)
        self.assertEqual(len([l for l in lines if l == 'TINYOS ready, 3 programs, 2 apps']), 2, lines)   # home, then home again
        until = lines.index('exit HELLO to TINYOS, depth 0')   # TINYOS gave back the panel and sound before it launched HELLO; the run's end cuts the second TINYOS off
        self.assertEqual([l for l in lines[:until] if 'left ' in l and 'open' in l or 'Uncaught' in l], [], lines)
        self.assertNotEqual(back, crc)    # HELLO is selected on the way back
        self.assertEqual(fixture('home-back-from-hello', back), back)
        self.assertEqual(lines.count('TINYOS audio underruns 0'), 1)   # the chime plays once, not again on the way back

    def test_an_app_that_cannot_run_is_listed_with_its_reason(self):
        _, lines, note, _, _ = boot(self.d, ['down'] * 3 + ['enter'], ms=1500)
        self.assertNotIn('hello OS ready', lines)
        self.assertFalse([l for l in lines if l.startswith('launch ')], lines)
        self.assertEqual(fixture('home-broken-note', note), note)
        _, _, home, _, _ = boot(self.d, ms=1000)
        self.assertNotEqual(note, home)

    def test_setting_is_saved_and_read_back_after_a_restart(self):
        _, _, home, _, _ = boot(self.d, ms=1000)
        rc, lines, crc, _, _ = boot(self.d, ['down', 'down', 'enter', 'right', 'down', 'left', 'esc', 'q'], ms=2000)
        self.assertEqual(rc, 0)
        self.clean(lines)
        saved = json.loads((self.d / 'OS/SAVES/TINYOS/SETTINGS.CFG').read_text())
        self.assertEqual(saved, {'theme': 1, 'volume': 5})
        _, lines, paper, _, _ = boot(self.d, ms=1000)   # a new start reads the file
        self.assertNotEqual(paper, home)
        self.assertEqual(fixture('home-paper', paper), paper)
        _, _, again, _, _ = boot(self.d, ['down', 'down', 'enter', 'esc', 'q'], ms=2000)   # leaving without a change keeps it
        self.assertEqual(json.loads((self.d / 'OS/SAVES/TINYOS/SETTINGS.CFG').read_text()), saved)

    def test_chime_has_no_underruns_and_makes_sound(self):
        rc, lines, _, _, au = boot(self.d, ms=1500)
        self.assertIn('TINYOS audio underruns 0', lines)
        self.assertGreater(len(au), 0)
        self.assertTrue(any(b for b in au))
        rc, lines, _, _, au = boot(self.d, ms=1500, extra=['--no-audio'])   # no sound device: still boots
        self.assertIn('TINYOS ready, 3 programs, 2 apps', lines)

    def test_eink_clock_is_drawn_and_updates_slowly(self):
        _, lines, _, pages, _ = boot(self.d, ms=1000)
        self.assertEqual(len(pages), 1)
        black = sum(bin(b).count('1') for b in pages[0])
        self.assertGreater(black, 400)
        self.assertEqual(fixture('eink-0000', zlib.crc32(pages[0])), zlib.crc32(pages[0]))
        _, _, _, pages, _ = boot(self.d, ms=125000)       # two virtual minutes: a refresh every 30 s, not every second
        self.assertTrue(3 <= len(pages) <= 6, len(pages))
        self.assertNotEqual(pages[0], pages[-1])
        _, lines, _, pages, _ = boot(self.d, ms=1000, extra=['--no-eink'])
        self.assertEqual(pages, [])
        self.assertIn('TINYOS ready, 3 programs, 2 apps', lines)

    def test_quit_leaves_nothing_open(self):
        rc, lines, _, _, _ = boot(self.d, ['q'], ms=3000)
        self.assertEqual(rc, 0)
        self.assertIn('app returned status 0', lines)
        self.clean(lines)


if __name__ == '__main__':
    RUN = sys.argv.pop(1)
    UPDATE = '--update' in sys.argv
    if UPDATE:
        sys.argv.remove('--update')
    unittest.main()
