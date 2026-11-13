#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests of the JS runtime: each case in cases.py runs headless (virtual clock) and must
log the same lines and draw the same framebuffer (size, depth, CRC-32) as the gamemaker-os build
did (tests/fixtures.json, recorded by tools/make_fixtures.py). Also: the DOM probe passes,
a run twice is byte-identical, and no case logs an uncaught error.
Usage: run.py <jsrun> [--ref <another jsrun>]  (--ref compares live instead of the fixtures)"""
import json, pathlib, re, sys, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cases

JSRUN = None
REF = None
FIX = json.loads((cases.JS / 'tests/fixtures.json').read_text())


# Slice 3 added the `sandbox` global, fs.open and the manifest natives, and slice 4a the file classes (budget 8 KiB to 12 KiB), and slice 4b the display, time and input bindings (budget 24 KiB), so the two heap figures in the log lines
# ("heap N bytes in M blocks") are a little larger than the recorded ones. They are compared as "within a budget of
# growth" instead of exactly; every other byte of the log lines, the framebuffer CRCs and the exit codes stay exact.
HEAP = re.compile(r'(?:heap|memory) (\d+) bytes(?: in (\d+) blocks)?(?:, \d+ objects)?')
HEAP_GROWTH_BYTES, HEAP_GROWTH_BLOCKS = 24576, 330


def heaps(lines):
    return [tuple(int(g) if g else 0 for g in m.groups()) for l in lines for m in [HEAP.search(l)] if m]


def same_but_heap(testcase, got, want):
    testcase.assertEqual([HEAP.sub('heap N', l) for l in got], [HEAP.sub('heap N', l) for l in want])
    for (gb, gk), (wb, wk) in zip(heaps(got), heaps(want)):
        testcase.assertTrue(0 <= gb - wb <= HEAP_GROWTH_BYTES, f'heap {gb} vs recorded {wb}')
        testcase.assertTrue(0 <= gk - wk <= HEAP_GROWTH_BLOCKS, f'blocks {gk} vs recorded {wk}')


class Fixtures(unittest.TestCase):
    def test_cases_match_the_gamemaker_os_build(self):
        for name in cases.CASES:
            with self.subTest(case=name):
                got = cases.run(JSRUN, name)
                want = cases.run(REF, name) if REF else FIX[name]
                same_but_heap(self, got['out'], want['out'])
                self.assertEqual(got['shot'], want['shot'])
                self.assertEqual(got['rc'], want['rc'])

    def test_deterministic(self):
        self.assertEqual(cases.run(JSRUN, 'hello'), cases.run(JSRUN, 'hello'))

    def test_no_errors_and_probe_passes(self):
        for name in cases.CASES:
            out = cases.run(JSRUN, name)['out']
            self.assertEqual([l for l in out if 'Uncaught' in l or 'FAIL' in l or 'JS error' in l], [], name)
        out = cases.run(JSRUN, 'probe')['out']
        self.assertIn('JS DONE', ' '.join(out))
        self.assertGreaterEqual(sum('JS PASS' in l for l in out), 19)

    def test_hello_mode(self):
        r = cases.run(JSRUN, 'hello')
        self.assertEqual((r['shot']['w'], r['shot']['h'], r['shot']['depth']), (960, 540, 16))
        self.assertIn('JS hello OS ready', r['out'])

    def test_bench_finishes(self):
        out = cases.run(JSRUN, 'bench')['out']
        self.assertTrue(any('BENCH done' in l for l in out))

    def test_core_profile_has_no_game(self):
        # the <game> element stays in the DOM subset; play() reports the missing profile
        self.assertTrue(any('no game' in l for l in cases.run(JSRUN, 'bench')['out']))


if __name__ == '__main__':
    args = sys.argv[1:]
    if '--ref' in args:
        i = args.index('--ref'); REF = args[i + 1]; del args[i:i + 2]
    JSRUN = args.pop(0)
    unittest.main(argv=['run'] + args, verbosity=2)
