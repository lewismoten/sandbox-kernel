#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""The profile interface (src/profile.h) with the test profile (tests/host/profile_test.c):
hooks run in order, the key routes work (host key, game.press), stop releases everything
(50 start/stop cycles, a flat live-block count), and the core profile (no game) leaks nothing
over 50 cycles either. Usage: profile_run.py <jsrun (core)> <jsrun-pt (test profile)>"""
import pathlib, re, shutil, subprocess, sys, tempfile, unittest

JS = pathlib.Path(__file__).resolve().parents[1]
CORE = PT = None


def card():
    d = pathlib.Path(tempfile.mkdtemp())
    (d / 'OS/APPS').mkdir(parents=True)
    (d / 'OS/APPS/PROFTEST').symlink_to(JS / 'tests/apps/PROFTEST')
    return d


def run(exe, args):
    d = card()
    try:
        p = subprocess.run([str(exe), str(d), '/sd/OS/APPS/PROFTEST'] + args + ['--vclock'], capture_output=True, text=True, timeout=300)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return p.returncode, (p.stdout + p.stderr).splitlines()


def tags(out):
    out = [l[3:] if l.startswith('JS APP') else l for l in out]  # console.log lines come with the runtime's "JS " prefix
    return [l for l in out if l.startswith(('PT ', 'APP '))]


class Profile(unittest.TestCase):
    def test_hooks_in_order_and_keys_route(self):
        rc, out = run(PT, ['--ms', '2500', '--keys', '200:30:1,210:30:0,400:1:1,410:1:0,1000:1:1'])
        t = tags(out)
        want = ['PT init', 'PT play-refused /sd/GAMES/BAD', 'APP refused RangeError: no game in /sd/GAMES/BAD',
                'PT play /sd/GAMES/T1', 'APP playing true', 'PT key 31 1',      # game.press
                'PT key 30 1', 'PT key 30 0', 'PT key 1 1',                      # the host's keys
                'PT end', 'APP ended 1 ticks']
        got = [re.sub(r'(ticks) \d+.*', r'\1', l) for l in t if not l.startswith(('PT picture', 'PT draw'))]
        self.assertEqual(got[:len(want)], want, '\n'.join(t))
        self.assertTrue(any(l.startswith('PT picture /sd/GAMES/PIC') for l in t), 'the picture provider ran')
        self.assertIn('PT draw', t, 'the draw hook ran in phase 2')
        ends = [i for i, l in enumerate(t) if l == 'PT end']
        self.assertEqual(len(ends), 2)
        self.assertTrue(t[-1].startswith('PT stop held=0'), t[-1])
        self.assertEqual(t.count('PT init'), 1)
        self.assertTrue(any(l.startswith('APP ended 2 ticks') for l in t))
        self.assertEqual(rc, 0)

    def test_stop_while_playing_ends_then_stops(self):
        rc, out = run(PT, ['--ms', '600'])  # never ended by a key: the core stops the element
        t = tags(out)
        self.assertEqual([l for l in t if l in ('PT init', 'PT end')] , ['PT init', 'PT end'])
        self.assertTrue(t[-1].startswith('PT stop held=0'), t[-1])
        self.assertEqual(t[-2], 'PT end')

    def test_inert_core_profile(self):
        rc, out = run(CORE, ['--ms', '600'])
        self.assertIn('APP refused TypeError: play(): this build has no game profile', tags(out))
        self.assertEqual([l for l in out if l.startswith('PT ')], [])
        self.assertEqual(rc, 0)

    def _flat(self, exe, args):
        rc, out = run(exe, args + ['--cycles', '50'])
        self.assertEqual(rc, 0, out)
        m = re.search(r'CYCLES 50 blocks first (\d+) last (\d+)', '\n'.join(out))
        self.assertTrue(m, out[-3:])
        self.assertEqual(m.group(1), m.group(2), 'live heap blocks grew over 50 start/stop cycles')
        return out

    def test_fifty_cycles_do_not_leak(self):
        out = self._flat(PT, ['--ms', '600'])  # always stopped while playing
        self.assertEqual(sum(l == 'PT init' for l in out), 50)
        stops = [l for l in out if l.startswith('PT stop')]
        self.assertEqual(len(stops), 50)
        self.assertTrue(all(l.startswith('PT stop held=0') for l in stops))
        self.assertEqual(sum(l == 'PT end' for l in out), sum(l.startswith('PT play ') for l in out))

    def test_fifty_cycles_ended_by_key(self):
        self._flat(PT, ['--ms', '2500', '--keys', '400:1:1,410:1:0,1000:1:1'])

    def test_fifty_cycles_core(self):
        self._flat(CORE, ['--ms', '600'])


if __name__ == '__main__':
    CORE, PT = sys.argv[1], sys.argv[2]
    del sys.argv[1:3]
    unittest.main()
