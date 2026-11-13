#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 5 tests: allocation failure. tests/apps/ALLOCS touches every place the runtime allocates natively (elements, text,
canvases, file handles, a growing writer, the display lease, timers, audio, e-ink, serial). The fault build (jsrun-fault) fails the
N-th allocation of one series for every N, each run in its own process, and the runtime must not crash or leak. Two series:
  direct  the C bindings' own malloc/calloc/realloc (src/alloc.h): every one of them is failed in turn;
  qjs     everything QuickJS asks for through the heap wrapper in jsrt.c (its arenas and big blocks).
Known and counted apart: a QuickJS 0.18.0 allocation failure inside its own compiler (while the runtime's prelude or the app's
script loads) crashes or leaks inside the engine (a NULL byte-code buffer in js_parse_class and in resolve_variables). The soft
limit in jsrt.c keeps the app limit from ever causing one; this test only pins that the count of such sites does not grow
unnoticed. Also a source audit: a new allocation site must be added to SITES below, which is when to give it a failure path.
Usage: alloc_run.py <jsrun-fault>"""
import pathlib, re, shutil, subprocess, sys, tempfile, unittest

JS = pathlib.Path(__file__).resolve().parents[1]
RUN = None
# allocation calls per source file (not counting jsrt.c's heap wrapper, which is the qjs series)
SITES = {'dom.c': 3, 'jseink.c': 1, 'jsaudio.c': 1, 'jsdev.c': 2, 'jsfs.c': 3, 'jsrt.c': 1, 'jsserial.c': 1}


def sweep(kind, extra=()):
    d = pathlib.Path(tempfile.mkdtemp())
    try:
        shutil.copytree(JS / 'tests/apps/ALLOCS', d / 'OS/APPS/ALLOCS')
        (d / 'OS/SAVES/ALLOCS').mkdir(parents=True)
        p = subprocess.run([str(RUN), str(d), '/sd/OS/APPS/ALLOCS', '--vclock', '--ms', '400', '--sweep', kind] + list(extra), capture_output=True, text=True, timeout=600)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    m = re.search(r'SWEEP %s total (\d+) runs (\d+) app_did_not_start (\d+) leaks_load (\d+) leaks_run (\d+) crashes_load (\d+) crashes_run (\d+)' % kind, p.stdout)
    assert m, (p.returncode, p.stdout[-600:], p.stderr[-300:])
    keys = ('total', 'runs', 'not_started', 'leaks_load', 'leaks_run', 'crashes_load', 'crashes_run')
    return p.returncode, dict(zip(keys, map(int, m.groups()))), p.stdout


class Alloc(unittest.TestCase):
    def test_every_direct_allocation_can_fail(self):
        rc, r, out = sweep('direct')
        self.assertGreaterEqual(r['total'], 30, r)   # the app reaches every site: elements, files, writer growth, devices
        self.assertEqual(r['runs'], r['total'])
        self.assertEqual((r['leaks_load'], r['leaks_run'], r['crashes_load'], r['crashes_run']), (0, 0, 0, 0), out[-800:])
        self.assertEqual(rc, 0)

    def test_every_quickjs_allocation_can_fail_once_the_app_has_loaded(self):
        rc, r, out = sweep('qjs')
        self.assertGreater(r['total'], 400, r)
        self.assertEqual(r['runs'], r['total'])
        self.assertEqual((r['leaks_run'], r['crashes_run']), (0, 0), out[-800:])
        self.assertLess(r['not_started'], r['runs'], 'no failure was ever reached after the app loaded')
        self.assertLessEqual(r['crashes_load'], 20, 'more compiler-time failures crash than before: ' + str(r))   # 13 with QuickJS-ng 0.18.0
        self.assertLessEqual(r['leaks_load'], 4, r)
        self.assertEqual(rc, 0)

    def test_every_allocation_site_in_the_sources_is_known(self):
        found = {}
        for f in sorted((JS / 'src').glob('*.c')):
            text = f.read_text()
            if f.name == 'jsrt.c':   # the heap wrapper's own calls are the JSRT_Q* macros, not these
                text = re.sub(r'JSRT_Q(MALLOC|CALLOC|REALLOC|FREE)', '', text)
            n = len(re.findall(r'\b(?:malloc|calloc|realloc|strdup)\s*\(', re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)))
            if n:
                found[f.name] = n
        self.assertEqual(found, SITES, 'an allocation site was added or removed: give it a failure path, extend tests/apps/ALLOCS to reach it, then update SITES')


if __name__ == '__main__':
    RUN = pathlib.Path(sys.argv.pop(1)).resolve()
    unittest.main(verbosity=1)
