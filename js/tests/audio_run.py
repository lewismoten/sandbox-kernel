#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 4c tests: audio (open rules, partial writes, ring-full, underruns, the refill pump through 100 ms sleep chunks, a long
sample in small writes, tones, conversion, close, stop releases it, 50 starts with no leaks), on the host model of the kernel's
48 kHz ring in tests/host/jsrun.c (virtual clock, so every number is exact). Usage: audio_run.py <jsrun>"""
import json, pathlib, re, shutil, struct, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import dev_run as D

AUD = ('audio', 'time')
PRE = D.PRE


def run(self, code, caps=AUD, args=(), ms='400', dump=False):
    extra = list(args)
    path = None
    if dump:
        path = pathlib.Path(tempfile.mkdtemp()) / 'pcm'
        extra += ['--audio-dump', str(path)]
    rc, t, rest, _ = D.go(code, caps=caps, args=extra, ms=ms)
    self.assertEqual(rc, 0, rest)
    pcm = None
    if path:
        raw = path.read_bytes() if path.exists() else b''
        pcm = list(struct.unpack('<%dh' % (len(raw) // 2), raw))
        shutil.rmtree(path.parent, True)
    return t, rest, pcm


class Open(unittest.TestCase):
    def test_open_rules(self):
        t, _, _ = run(self, PRE + """
E('badrate', () => audio.open({ rate: 44100 }));
E('badchannels', () => audio.open({ channels: 3 }));
E('badtarget', () => audio.open({ targetMs: 5 }));
const a = audio.open({ rate: 24000, channels: 1 });
E('second', () => audio.open());
E('badpcm', () => a.write(new Float32Array(4)));
E('hz', () => a.tone(10, 100));
E('ms', () => a.tone(440, 0));
E('volume', () => { a.volume = 2; });
const s = a.stats(); T('s', [s.rate, s.capacity, s.appRate, s.channels].join(','));
a.close(); a.close();
E('afterclose', () => a.write(new Int16Array(2)));
E('reopen', () => audio.open().close());
E('oddframes', () => { const b = audio.open(); try { b.write(new Int16Array(3)); } finally { b.close(); } });
sys.exit(0);
""")
        self.assertTrue(t['badrate'].startswith('RangeError') or 'EINVAL' in t['badrate'], t['badrate'])
        for k in ('badchannels', 'badtarget', 'hz', 'ms', 'volume'):
            self.assertIn('EINVAL', t[k], k)
        self.assertIn('EBUSY', t['second'])
        self.assertIn('Int16Array', t['badpcm'])
        self.assertIn('EINVAL', t['oddframes'])
        self.assertEqual(t['s'], '48000,16384,24000,1')
        self.assertIn('EBADF', t['afterclose'])
        self.assertTrue(t['reopen'].startswith('ok'), t['reopen'])

    def test_no_device_and_foreign_writer(self):
        t, _, _ = run(self, PRE + "E('open', () => audio.open()); sys.exit(0);", args=['--no-audio'])
        self.assertIn('ENODEV', t['open'])
        t, _, _ = run(self, PRE + "E('open', () => audio.open()); sys.exit(0);", args=['--audio-busy'])
        self.assertIn('EBUSY', t['open'])

    def test_capability(self):
        t, _, _ = run(self, PRE + "E('open', () => audio.open()); sys.exit(0);", caps=('time',))
        self.assertIn('audio', t['open'])


class Ring(unittest.TestCase):
    def test_partial_write_and_ring_full(self):
        t, _, _ = run(self, PRE + """
const a = audio.open();
const big = new Int16Array(20000 * 2);
T('first', a.write(big));            // the ring holds 16384 frames
T('second', a.write(big));           // full: nothing taken, not an error
let s = a.stats(); T('full', [s.queued, s.overruns, s.overrunFrames, s.refused].join(','));
sleep(100).then(() => {              // 100 ms drains exactly 4800 frames
  T('third', a.write(big));
  s = a.stats(); T('after', [s.queued, s.played].join(','));
  sys.exit(0);
});
""")
        self.assertEqual(t['first'], '16384')
        self.assertEqual(t['second'], '0')
        self.assertEqual(t['full'], '16384,0,0,2')  # the runtime clips to the room it sees, so the kernel never refuses a write
        self.assertEqual(t['third'], '4800')
        self.assertEqual(t['after'], '16384,4800')

    def test_no_underrun_before_first_write_then_counted_when_the_app_stops_feeding(self):
        t, _, _ = run(self, PRE + """
const a = audio.open();
sleep(100).then(() => {
  T('idle', a.stats().underruns);
  a.write(new Int16Array(4800 * 2));  // 100 ms of sound
  return sleep(300);
}).then(() => { const s = a.stats(); T('dry', [s.underruns, s.queued, s.played].join(',')); sys.exit(0); });
""")
        self.assertEqual(t['idle'], '0')
        self.assertEqual(t['dry'], '200,0,4800')  # 200 ms of silence: one underrun per dry millisecond tick


class Pump(unittest.TestCase):
    def test_pump_keeps_the_ring_fed_through_100ms_sleep_chunks(self):
        t, _, _ = run(self, PRE + """
const a = audio.open({ onneed(frames) { a.write(new Int16Array(frames * 2).fill(1000)); } });
// an app that is busy sleeping in 100 ms chunks of its own: the pump must still run between them
(async () => { for (let i = 0; i < 30; i++) await sleep(100); const s = a.stats();
  T('s', [s.underruns, s.played > 120000, s.refills > 10, s.gaps].join(',')); sys.exit(0); })();
""", ms='3500', args=['--sleepstat'])
        self.assertEqual(t['s'], '0,true,true,0')

    def test_a_long_sample_in_small_writes_never_exceeds_the_heartbeat(self):
        t, rest, _ = run(self, PRE + """
const sample = new Int16Array(480000 * 2);   // ten seconds
let at = 0;
const a = audio.open({ onneed(frames) {
  const n = Math.min(frames, 256, 480000 - at);   // small writes
  at += a.write(sample.subarray(at * 2, (at + n) * 2));
} });
sleep(6000).then(() => { const s = a.stats(); T('s', [s.underruns, at > 250000, s.writes > 1000].join(',')); sys.exit(0); });
""", ms='6500', args=['--sleepstat', '--heap', str(3 << 20)])   # a 1.9 MB sample is past the app default (1.5 MiB)
        self.assertEqual(t['s'], '0,true,true')
        m = re.search(r'HOST sleeps (\d+) max_us (\d+)', '\n'.join(rest))
        self.assertTrue(m, rest)
        self.assertLessEqual(int(m.group(2)), 100000, 'the loop slept longer than the 100 ms chunk (the heartbeat limit is 2 s)')

    def test_onneed_error_is_reported_and_the_pump_goes_on(self):
        t, rest, _ = run(self, PRE + """
let n = 0;
const a = audio.open({ onneed() { if (n++ < 2) throw new Error('boom'); a.write(new Int16Array(960)); } });
sleep(300).then(() => { T('n', n > 3); sys.exit(0); });
""", args=[])
        self.assertEqual(t['n'], 'true')
        self.assertTrue(any('boom' in l for l in rest))

    def test_no_pump_without_onneed(self):
        t, _, _ = run(self, PRE + """
const a = audio.open();
sleep(300).then(() => { T('r', a.stats().refills); sys.exit(0); });
""")
        self.assertEqual(t['r'], '0')


class Convert(unittest.TestCase):
    def test_mono_rate_and_volume(self):
        t, _, pcm = run(self, PRE + """
const a = audio.open({ rate: 24000, channels: 1 });
T('n', a.write(Int16Array.of(1000, -1000)));
a.volume = 0.5; a.write(Int16Array.of(1000));
a.muted = true; a.write(Int16Array.of(1000));
a.muted = false; a.volume = 1; a.write(Int16Array.of(7));
sys.exit(0);
""", dump=True)
        self.assertEqual(t['n'], '2')
        want = [1000, 1000] * 2 + [-1000, -1000] * 2 + [500, 500] * 2 + [0, 0] * 2 + [7, 7] * 2
        self.assertEqual(pcm, want)

    def test_partial_write_at_a_repeat_rate_counts_app_frames(self):
        t, _, _ = run(self, PRE + """
const a = audio.open({ rate: 8000, channels: 2 });   // x6: 16384 kernel frames = 2730 app frames
T('n', a.write(new Int16Array(5000 * 2)));
sys.exit(0);
""")
        self.assertEqual(t['n'], '2730')


class Tones(unittest.TestCase):
    def test_square_wave_frequency_rests_and_exclusion(self):
        t, _, pcm = run(self, PRE + """
const a = audio.open();
a.tone(440, 100); a.tone(0, 50); a.tone(880, 100, 0.5);
T('pending', a.stats().pendingTones);
T('write', a.write(new Int16Array(200)));   // tones own the ring until they finish
sleep(500).then(() => { const s = a.stats(); T('end', [s.pendingTones, s.underruns > 0, s.played].join(',')); sys.exit(0); });
""", dump=True, ms='700')
        self.assertEqual(t['pending'], '3')
        self.assertEqual(t['write'], '0')
        left = pcm[0::2]
        self.assertEqual(len(left), (100 + 50 + 100) * 48)
        a, rest, b = left[:4800], left[4800:7200], left[7200:]
        self.assertEqual(set(a), {8192, -8192})
        self.assertEqual(set(rest), {0})
        self.assertEqual(set(b), {4096, -4096})
        flips = lambda x: sum(1 for i in range(1, len(x)) if x[i] != x[i - 1])
        self.assertIn(flips(a), (87, 88, 89))     # 440 Hz for 0.1 s: 88 edges
        self.assertIn(flips(b), (175, 176, 177))
        self.assertEqual(t['end'].split(',')[0], '0')
        self.assertEqual(pcm[0::2][4800 + 2400 - 1], 0)
        self.assertEqual(pcm[1::2], pcm[0::2], 'a tone is the same in both channels')

    def test_tone_queue_is_bounded_and_stop_forgets_it(self):
        t, _, _ = run(self, PRE + """
const a = audio.open();
let n = 0; for (; n < 100; n++) { try { a.tone(440, 1000); } catch (e) { T('full', e.message.slice(0, 6)); break; } }
T('n', n);
a.stop(); T('after', a.stats().pendingTones);
sys.exit(0);
""")
        self.assertEqual(t['n'], '32')
        self.assertEqual(t['full'], 'EMFILE')
        self.assertEqual(t['after'], '0')

    def test_a_long_melody_keeps_the_ring_fed(self):
        t, _, _ = run(self, PRE + """
const a = audio.open();
for (let i = 0; i < 20; i++) a.tone(262 + i * 30, 200);   // four seconds, 20 tones
sleep(4300).then(() => { const s = a.stats(); T('s', [s.pendingTones, s.played >= 4 * 48000 - 4800, s.underruns].join(',')); sys.exit(0); });
""", ms='4500', args=['--sleepstat'])
        p, ok_, under = t['s'].split(',')
        self.assertEqual((p, ok_), ('0', 'true'))
        self.assertEqual(int(under), 300, 'the only dry milliseconds are the 300 after the last tone ended')


class Leaks(unittest.TestCase):
    def cycles(self, code, caps=AUD):
        d = pathlib.Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, d, True)
        (d / 'OS/APPS/DT').mkdir(parents=True)
        (d / 'OS/APPS/DT/APP.JSON').write_text(json.dumps({'name': 'Dev', 'api': 1, 'capabilities': list(caps)}))
        (d / 'OS/APPS/DT/MAIN.JS').write_text(code)
        p = subprocess.run([str(D.RUN), str(d), '/sd/OS/APPS/DT', '--cycles', '50', '--ms', '150', '--vclock'], capture_output=True, text=True, timeout=300)
        m = re.search(r'CYCLES (\d+) blocks first (\d+) last (\d+) leaks (\d+) open (\d+)', p.stdout)
        self.assertTrue(m, p.stdout[-300:] + p.stderr[-300:])
        return tuple(map(int, m.groups())), p.stdout

    def test_tidy_app_50_starts_no_leaks(self):
        (n, first, last, leaks, opened), _ = self.cycles("""
const a = audio.open({ onneed(f) { a.write(new Int16Array(f * 2)); } }); a.tone(440, 20);
sleep(60).then(() => a.close());
""")
        self.assertEqual((n, leaks, opened), (50, 0, 0)); self.assertEqual(first, last, 'live heap blocks grew')

    def test_careless_app_50_starts_stop_releases_the_device(self):
        (n, first, last, leaks, opened), out = self.cycles("""
globalThis.a = audio.open({ onneed(f) { a.write(new Int16Array(f * 2)); } });
a.tone(440, 5000); sleep(900);
""")
        self.assertEqual((n, opened), (50, 0)); self.assertEqual(leaks, 100, 'the device and the sleep timer each start')
        self.assertEqual(first, last, 'live heap blocks grew')
        self.assertIn('left 2 handle(s) open: audio 1, timer 1', out)

    def test_the_device_is_free_for_the_next_app(self):
        # 50 starts through the cycles above would fail with EBUSY on the 2nd start if stop kept the device
        t, _, _ = run(self, PRE + "const a = audio.open(); T('ok', 1); sys.exit(0);")
        self.assertEqual(t['ok'], '1')


if __name__ == '__main__':
    D.RUN = pathlib.Path(sys.argv.pop(1)).resolve()
    unittest.main()
