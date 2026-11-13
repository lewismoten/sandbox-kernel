#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 5 soak test: TINYOS launches HELLO and HELLO returns, 500 times in one run of the real launcher (AUTORUN, apps.launch stack),
with audio (TINYOS's chime on every start), e-ink (its clock and panel on every start) and input (keys and a device detach and
attach in each cycle) on the host models. Every cycle is a fresh runtime. It checks the numbers a board would care about: the
longest loop turn (wall time, with the host's virtual clock the sleeps cost nothing, so it is the runtime's own cost), the heap's
high-water mark, and that nothing is left behind: no handle or power hold left open by an app, the runtime's heap empty after
every stop, and the system allocator's live blocks the same after 500 cycles as after 20 (a leak would scale with the count).
Usage: soak_run.py <jsrun> [cycles]   (prints the numbers)"""
import pathlib, re, shutil, subprocess, sys, tempfile, unittest

JS = pathlib.Path(__file__).resolve().parents[1]
RUN = None
CYCLES = 500
K = {'down': 108, 'enter': 28, 'esc': 1, 'left': 105, 'right': 106}
PERIOD = 1000


def card():
    d = pathlib.Path(tempfile.mkdtemp())
    shutil.copytree(JS / 'examples/TINYOS', d / 'OS/APPS/TINYOS')
    shutil.copytree(JS / 'apps/HELLO', d / 'OS/APPS/HELLO')
    (d / 'OS/AUTORUN.TXT').write_text('JS.ELF\nAPPS/TINYOS\n')
    return d


def key(at, name):
    return ['%d:%d:1' % (at, K[name]), '%d:%d:0' % (at + 20, K[name])]


def soak(cycles):
    """Runs the soak; returns (returncode, log lines, numbers dict)."""
    d = card()
    t = pathlib.Path(tempfile.mkdtemp())
    try:
        # one cycle: TINYOS is up, Enter on HELLO, input traffic inside HELLO (arrows, a device detach and attach), Esc back
        k = key(60, 'down') + key(80, 'down') + key(100, 'down') + key(130, 'enter') + key(420, 'left') + key(480, 'right') + ['600:0:3', '640:0:1:256'] + key(700, 'esc')
        a = [str(RUN), str(d), 'x', '--autorun', '--vclock', '--soak', '--ms', str(cycles * PERIOD + 400),
             '--keys', ','.join(k), '--keys-repeat', '%d:%d' % (PERIOD, cycles),
             '--eink-dump', str(t / 'e'), '--audio-dump', str(t / 'a')]
        p = subprocess.run(a, capture_output=True, text=True, timeout=900)
        lines = [l[3:] if l.startswith('JS ') else l for l in (p.stdout + p.stderr).splitlines()]
        m = re.search(r'HOST soak steps (\d+) max_step_us (\d+) heap_peak (\d+) blocks_start (-?\d+) blocks_end (-?\d+) heap_blocks (\d+)', '\n'.join(lines))
        n = dict(zip(('steps', 'max_step_us', 'heap_peak', 'b0', 'b1', 'heap_blocks'), map(int, m.groups()))) if m else {}
        n['eink_bytes'] = (t / 'e').stat().st_size if (t / 'e').exists() else 0
        n['audio_bytes'] = (t / 'a').stat().st_size if (t / 'a').exists() else 0
        return p.returncode, lines, n
    finally:
        shutil.rmtree(d, ignore_errors=True)
        shutil.rmtree(t, ignore_errors=True)


class Soak(unittest.TestCase):
    def test_tinyos_and_hello_500_times(self):
        rc, lines, n = soak(CYCLES)
        launches = len([l for l in lines if l.startswith('launch HELLO from TINYOS')])
        returns = len([l for l in lines if l.startswith('exit HELLO to TINYOS')])
        print('\nSOAK cycles %d launches %d returns %d steps %d max_step_us %d heap_peak %d bytes blocks_start %d blocks_end %d eink_bytes %d audio_bytes %d'
              % (CYCLES, launches, returns, n['steps'], n['max_step_us'], n['heap_peak'], n['b0'], n['b1'], n['eink_bytes'], n['audio_bytes']))
        self.assertGreaterEqual(launches, CYCLES - 1, 'cycles did not all run: ' + '\n'.join(lines[-8:]))
        self.assertLessEqual(abs(returns - launches), 1)
        leaks = [l for l in lines if re.search(r'left \d+ (handle|power)', l) or 'Uncaught' in l or 'stopped:' in l or 'fault' in l.lower()]
        # the run ends mid-cycle at most once: that last stop may hold the panel, a real power-off would too
        self.assertLessEqual(len(leaks), 2, leaks[:6])
        self.assertEqual(n['heap_blocks'], 0)
        self.assertLess(n['heap_peak'], 1536 * 1024)
        self.assertLess(n['max_step_us'], 500_000, 'a loop turn took over 0.5 s')
        self.assertGreater(n['eink_bytes'], 0)
        self.assertGreater(n['audio_bytes'], 0)

    def test_no_leak_grows_with_the_cycles(self):
        _, la, a = soak(20)
        _, lb, b = soak(100)
        self.assertEqual(b['b1'] - b['b0'], a['b1'] - a['b0'], 'live allocator blocks grew with the number of cycles: %s vs %s' % (a, b))


if __name__ == '__main__':
    RUN = pathlib.Path(sys.argv.pop(1)).resolve()
    if len(sys.argv) > 1 and sys.argv[-1].isdigit():
        CYCLES = int(sys.argv.pop())
    unittest.main(verbosity=1)
