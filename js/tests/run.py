#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Host tests of the JS runtime: each case in cases.py runs headless (virtual clock) and must
log the same lines and draw the same framebuffer (size, depth, CRC-32) as the gamemaker-os build
did (tests/fixtures.json, recorded by tools/make_fixtures.py). Also: the DOM probe passes,
a run twice is byte-identical, and no case logs an uncaught error.
Usage: run.py <jsrun> [--ref <another jsrun>]  (--ref compares live instead of the fixtures)"""
import json, pathlib, sys, unittest
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import cases

JSRUN = None
REF = None
FIX = json.loads((cases.JS / 'tests/fixtures.json').read_text())


class Fixtures(unittest.TestCase):
    def test_cases_match_the_gamemaker_os_build(self):
        for name in cases.CASES:
            with self.subTest(case=name):
                got = cases.run(JSRUN, name)
                want = cases.run(REF, name) if REF else FIX[name]
                self.assertEqual(got['out'], want['out'])
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
