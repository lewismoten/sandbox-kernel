#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 4d tests: e-ink (one owner, 1-bit pages, regions, the refresh limits with a queue that keeps only the newest page), serial
(partial reads and writes, the 'data' event, a flood), power (holds, tokens, the blank state and its events, the audio pump stopping while
the screen is dark), release at stop and 50 starts with no leaks, on the host models in tests/host/jsrun.c (virtual clock, so every number
is exact). Usage: devices_run.py <jsrun>"""
import json, pathlib, re, shutil, struct, subprocess, sys, tempfile, unittest
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import dev_run as D

PRE = D.PRE
ALLD = ('eink', 'serial', 'power', 'audio', 'time')
PAGE = 3904


class R:
    """One run: t (T lines), rest (other log lines), eink records [(kind, ms, page)], serial tx bytes, dev (HOST dev line as a dict)."""
    pass


def run(self, code, caps=ALLD, args=(), ms='400'):
    d = pathlib.Path(tempfile.mkdtemp())
    self.addCleanup(shutil.rmtree, d, True)
    extra = list(args) + ['--eink-dump', str(d / 'ei'), '--serial-dump', str(d / 'se'), '--devstat']
    rc, t, rest, _ = D.go(code, caps=caps, args=extra, ms=ms)
    r = R()
    r.rc, r.t, r.rest = rc, t, rest
    raw = (d / 'ei').read_bytes() if (d / 'ei').exists() else b''
    r.eink = [(struct.unpack_from('<II', raw, i), raw[i + 8:i + 8 + PAGE]) for i in range(0, len(raw), 8 + PAGE)]
    r.eink = [(k, m, pg) for (k, m), pg in r.eink]
    r.serial = (d / 'se').read_bytes() if (d / 'se').exists() else b''
    line = [l for l in rest if l.startswith('HOST dev')]
    r.dev = {k: int(v) for k, v in re.findall(r'(\w+) (\d+)', line[0][len('HOST dev'):])} if line else {}
    return r


def px(page, x, y):
    return bool(page[y * 32 + (x >> 3)] & (0x80 >> (x & 7)))


class EinkOpen(unittest.TestCase):
    def test_open_rules(self):
        r = run(self, PRE + """
T('avail', eink.available);
const p = eink.open();
T('avail2', eink.available);
T('info', [p.info.width, p.info.height, p.info.pageBytes, p.info.minIntervalMs, p.info.maxPerMinute].join(','));
E('second', () => eink.open());
E('short', () => p.page(new Uint8Array(100)));
E('type', () => p.page(new Uint16Array(1952)));
E('title', () => p.setTitle('bad~name'));
E('title0', () => p.setTitle(''));
E('mark', () => p.setTitle('OK', { mark: new Uint8Array(3) }));
p.close(); p.close();
E('closed', () => p.page(new Uint8Array(3904)));
E('reopen', () => { const q = eink.open(); q.close(); return 'again'; });
sys.exit(0);
""")
        t = r.t
        self.assertEqual((t['avail'], t['avail2']), ('true', 'true'))
        self.assertEqual(t['info'], '250,122,3904,1000,30')
        self.assertIn('EBUSY', t['second'])
        for k in ('short', 'title', 'title0', 'mark'):
            self.assertIn('EINVAL', t[k], k)
        self.assertIn('Uint8Array', t['type'] + t.get('type', ''))
        self.assertIn('EBADF', t['closed'])
        self.assertEqual(t['reopen'], 'ok again')
        self.assertEqual(r.dev['eink_open'], 0)

    def test_no_panel_and_foreign_owner_and_capability(self):
        r = run(self, PRE + "T('avail', eink.available); T('open', eink.open()); sys.exit(0);", args=['--no-eink'])
        self.assertEqual((r.t['avail'], r.t['open']), ('false', 'null'))
        r = run(self, PRE + "E('open', () => eink.open()); sys.exit(0);", args=['--eink-busy'])
        self.assertIn('EBUSY', r.t['open'])
        r = run(self, PRE + "E('open', () => eink.open()); E('avail', () => eink.available); sys.exit(0);", caps=('time',))
        self.assertIn('eink', r.t['open'])
        self.assertIn('eink', r.t['avail'])


class EinkPacing(unittest.TestCase):
    def test_first_page_goes_at_once_later_ones_are_queued_and_only_the_newest_is_sent(self):
        r = run(self, PRE + """
const p = eink.open(), c = p.canvas();
const flush = (n) => { c.clear(); c.fillRect(0, 0, n, 1); return p.flush(); };
T('r', [flush(1), flush(2), flush(3), flush(4)].join(','));
{ const s = p.stats(); T('s0', JSON.stringify({ sent: s.sent, pending: s.pending, replaced: s.replaced })); }
sleep(1500).then(() => { { const s = p.stats(); T('s1', JSON.stringify({ sent: s.sent, pending: s.pending })); } sys.exit(0); });
""", ms='3000')
        self.assertEqual(r.t['r'], 'true,false,false,false')
        s0 = json.loads(r.t['s0'])
        self.assertEqual((s0['sent'], s0['pending'], s0['replaced']), (1, 1, 2))
        s1 = json.loads(r.t['s1'])
        self.assertEqual((s1['sent'], s1['pending']), (2, 0))
        pages = [rec for rec in r.eink if rec[0] == 0]
        self.assertEqual(len(pages), 2)
        self.assertEqual(sum(bin(b).count('1') for b in pages[0][2]), 1)  # the first page, 1 pixel
        self.assertEqual(sum(bin(b).count('1') for b in pages[1][2]), 4)  # the newest page, not the 2- or 3-pixel ones
        self.assertGreaterEqual(pages[1][1] - pages[0][1], 1000)
        self.assertEqual(r.dev['eink_eagain'], 0, 'the runtime paces itself: the kernel is not asked too soon')

    def test_a_page_every_100_ms_never_beats_the_panel_limit(self):
        r = run(self, PRE + """
const p = eink.open(), c = p.canvas();
let i = 0;
(async () => { for (; i < 70; i++) { c.clear(); c.fillRect(0, 0, 1 + i, 1); p.flush(); await sleep(100); } T('last', i); sys.exit(0); })();
""", ms='9000')
        pages = [m for k, m, _ in r.eink if k == 0]
        self.assertTrue(4 <= len(pages) <= 8, pages)
        for a, b in zip(pages, pages[1:]):
            self.assertGreaterEqual(b - a, 1000)
        self.assertEqual(r.dev['eink_eagain'], 0)
        last = [p for k, _, p in r.eink if k == 0][-1]
        self.assertGreaterEqual(sum(bin(b).count('1') for b in last), 60, 'the newest pages win')

    def test_30_a_minute_is_kept_without_asking_the_kernel(self):
        r = run(self, PRE + """
const p = eink.open(), c = p.canvas();
(async () => { for (let i = 0; i < 700; i++) { c.setPixel(i % 250, 5); p.flush(); await sleep(100); } sys.exit(0); })();
""", ms='72000')
        pages = [m for k, m, _ in r.eink if k == 0]
        for i, a in enumerate(pages):
            self.assertLessEqual(len([b for b in pages[i:] if b < a + 60000]), 30, 'more than 30 in a 60 s window')
        self.assertGreaterEqual(len(pages), 40)
        self.assertEqual(r.dev['eink_eagain'], 0)

    def test_a_kernel_eagain_is_honored_and_retried_gently(self):
        r = run(self, PRE + """
const p = eink.open(), c = p.canvas();
(async () => { for (let i = 0; i < 60; i++) { c.setPixel(i, 0); p.flush(); await sleep(100); } T('s', p.stats().eagain); sys.exit(0); })();
""", ms='8000', args=['--eink-min-ms', '2500'])
        pages = [m for k, m, _ in r.eink if k == 0]
        self.assertGreaterEqual(len(pages), 2)
        for a, b in zip(pages, pages[1:]):
            self.assertGreaterEqual(b - a, 2500)
        eagain = int(r.t['s'])
        self.assertGreater(eagain, 0)
        self.assertLessEqual(eagain, 8000 // 250 + 2, 'retries are 250 ms apart, never a spin')
        self.assertIn(r.dev['eink_eagain'] - eagain, (0, 1), 'the one extra is the clear tried at stop')


class EinkDrawing(unittest.TestCase):
    def test_regions_and_bounds(self):
        r = run(self, PRE + """
const p = eink.open();
// a 10x3 bitmap at x=3 (not byte aligned), 2 bytes a row: top row all black, middle row white, bottom row first pixel and last pixel black
const bm = new Uint8Array([0xff, 0xc0, 0x00, 0x00, 0x80, 0x40]);
p.update(3, 5, 10, 3, bm, { send: false });
E('okedge', () => p.update(240, 121, 10, 1, new Uint8Array([0xff, 0xc0]), { send: false }));
E('right', () => p.update(241, 0, 10, 1, new Uint8Array([0xff, 0xc0])));
E('bottom', () => p.update(0, 121, 8, 2, new Uint8Array(2)));
E('neg', () => p.update(-1, 0, 8, 1, new Uint8Array(1)));
E('zero', () => p.update(0, 0, 0, 1, new Uint8Array(0)));
E('float', () => p.update(0.5, 0, 8, 1, new Uint8Array(1)));
E('size', () => p.update(0, 0, 10, 2, new Uint8Array(3)));
E('type', () => p.update(0, 0, 8, 1, [0]));
T('first', p.flush());
const b = p.bits;
T('count', b.reduce((n, v) => n + v.toString(2).replace(/0/g, '').length, 0));
sys.exit(0);
""")
        t = r.t
        self.assertEqual(t['okedge'], 'ok ')
        for k in ('right', 'bottom', 'neg', 'zero', 'float', 'size', 'type'):
            self.assertIn('EINVAL', t[k], k)
        page = [p for k, _, p in r.eink if k == 0][0]
        self.assertEqual(t['count'], str(10 + 2 + 10))
        for x in range(3, 13):
            self.assertTrue(px(page, x, 5), x)
            self.assertFalse(px(page, x, 6), x)
        self.assertTrue(px(page, 3, 7) and px(page, 12, 7) and not px(page, 4, 7))
        self.assertFalse(px(page, 2, 5) or px(page, 13, 5))  # neighbors untouched
        for x in range(240, 250):
            self.assertTrue(px(page, x, 121), x)
        self.assertFalse(px(page, 239, 121))

    def test_canvas_clips_and_whole_page_round_trips(self):
        r = run(self, PRE + """
const p = eink.open(), c = p.canvas();
c.fillRect(-5, -5, 10, 10); c.fillRect(245, 117, 20, 20); c.setPixel(-1, 3); c.setPixel(250, 3); c.setPixel(100, 121);
c.fillRect(1, 1, 2, 2, false);
p.flush();
sleep(1100).then(() => { const full = new Uint8Array(3904).fill(0x55); T('full', p.page(full)); T('same', p.bits.every((v) => v === 0x55)); sys.exit(0); });
""", ms='2000')
        page = [p for k, _, p in r.eink if k == 0][0]
        self.assertEqual(sum(bin(b).count('1') for b in page), 25 - 4 + 25 + 1)
        self.assertTrue(px(page, 0, 0) and not px(page, 1, 1) and px(page, 249, 121) and px(page, 100, 121))
        self.assertEqual((r.t['full'], r.t['same']), ('true', 'true'))
        self.assertEqual([p for k, _, p in r.eink if k == 0][1], b'\x55' * PAGE)

    def test_title_goes_through_the_same_limits(self):
        r = run(self, PRE + """
const p = eink.open();
T('a', p.setTitle('My OS', { build: 'V0.1' }));
T('b', p.setTitle('My OS 2'));
sleep(1200).then(() => sys.exit(0));
""", ms='2000')
        self.assertEqual((r.t['a'], r.t['b']), ('true', 'false'))
        self.assertEqual([k for k, _, _ in r.eink], [2, 2])


class EinkRelease(unittest.TestCase):
    def test_stop_gives_the_panel_back_with_a_clear(self):
        r = run(self, PRE + "const p = eink.open(); p.canvas().setPixel(1, 1); p.flush(); sleep(1500).then(() => { T('x', 1); sys.exit(0); });", ms='3000')
        self.assertEqual([k for k, _, _ in r.eink], [0, 1])
        self.assertEqual(r.dev['eink_open'], 0)

    def test_release_clears_and_close_keeps_the_page(self):
        r = run(self, PRE + "const p = eink.open(); p.flush(); sleep(1200).then(() => { p.release(); T('k', 1); sys.exit(0); });", ms='3000')
        self.assertEqual([k for k, _, _ in r.eink], [0, 1])
        r = run(self, PRE + "const p = eink.open(); p.flush(); sleep(1200).then(() => { p.close(); T('k', 1); sys.exit(0); });", ms='3000')
        self.assertEqual([k for k, _, _ in r.eink], [0])
        self.assertEqual(r.dev['eink_open'], 0)

    def test_a_queued_page_is_dropped_at_stop_not_sent_late(self):
        r = run(self, PRE + "const p = eink.open(), c = p.canvas(); c.setPixel(1, 1); p.flush(); c.setPixel(2, 2); p.flush(); sleep(100).then(() => sys.exit(0));", ms='3000')
        self.assertEqual([k for k, _, _ in r.eink], [0], 'too soon for the second page and for the clear: the kernel clears at the end of the program')


class SerialTests(unittest.TestCase):
    def test_open_rules(self):
        r = run(self, PRE + """
E('badmode', () => serial.open({ mode: 'x' }));
const s = serial.open({ mode: 'r' });
E('second', () => serial.open());
E('writeonread', () => s.write(new Uint8Array(1)));
E('badbuf', () => s.read([]));
E('onbad', () => s.on('line', () => {}));
s.close(); s.close();
E('closed', () => s.read(new Uint8Array(1)));
const w = serial.open({ mode: 'w' });
E('readonwrite', () => w.read(new Uint8Array(1)));
E('dataonwrite', () => w.on('data', () => {}));
w.close();
sys.exit(0);
""")
        t = r.t
        self.assertIn('EINVAL', t['badmode'])
        self.assertIn('EBUSY', t['second'])
        self.assertIn('EBADF', t['writeonread'])
        self.assertIn('Uint8Array', t['badbuf'])
        self.assertIn('data', t['onbad'])
        self.assertIn('EBADF', t['closed'])
        self.assertIn('EBADF', t['readonwrite'])
        self.assertIn('EBADF', t['dataonwrite'])
        self.assertEqual(r.dev['serial_open'], 0)

    def test_no_device_and_capability(self):
        r = run(self, PRE + "E('open', () => serial.open()); sys.exit(0);", args=['--no-serial'])
        self.assertIn('ENODEV', r.t['open'])
        r = run(self, PRE + "E('open', () => serial.open()); sys.exit(0);", caps=('time',))
        self.assertIn('serial', r.t['open'])

    def test_partial_reads(self):
        r = run(self, PRE + """
const s = serial.open();
const b = new Uint8Array(3), out = [];
out.push(s.read(b));                       // nothing yet
sleep(200).then(() => {
  const n1 = s.read(b); out.push(n1, [...b].join('.'));
  const n2 = s.read(b); out.push(n2, b[0], b[1]);
  out.push(s.read(b), s.read(new Uint8Array(0)));
  T('out', out.join(' ')); const st = s.stats(); T('st', [st.rxBytes, st.appRead, st.rxPending].join(','));  sys.exit(0);
});
""", args=['--serial-rx', '100:48656c6c6f'])
        self.assertEqual(r.t['out'], '0 3 72.101.108 2 108 111 0 0')
        self.assertEqual(r.t['st'], '5,5,0')

    def test_data_event_off_and_close(self):
        r = run(self, PRE + """
const s = serial.open({ mode: 'r' });
let got = '';
const f = (d) => { got += String.fromCharCode(...d); };
s.on('data', f);
sleep(250).then(() => { s.off('data', f); T('a', got); return sleep(300); })
  .then(() => { T('b', got); s.on('data', f); return sleep(200); })
  .then(() => { T('c', got); s.close(); sys.exit(0); });
""", ms='1200', args=['--serial-rx', '50:6162,200:63,300:64,500:65'])
        self.assertEqual((r.t['a'], r.t['b']), ('abc', 'abc'))  # while unsubscribed, the byte stays in the link (d) and arrives with e
        self.assertEqual(r.t['c'], 'abcde')

    def test_flood_is_bounded_and_the_loop_keeps_running(self):
        # 64 bursts of 64 bytes in 64 ms: the link holds 256, drops the rest and counts it; events are at most 256 bytes
        bursts = ','.join('%d:%s' % (100 + i, '41' * 64) for i in range(64))
        r = run(self, PRE + """
const s = serial.open({ mode: 'r' });
let total = 0, biggest = 0, events = 0, ticks = 0;
s.on('data', (d) => { total += d.length; events++; if (d.length > biggest) biggest = d.length; });
(async () => { for (let i = 0; i < 30; i++) { await sleep(20); ticks++; } const st = s.stats();
  T('o', [total, biggest, events > 0, ticks, st.rxBytes + st.rxDropped].join(',')); sys.exit(0); })();
""", ms='1500', args=['--serial-rx', bursts])
        total, biggest, events, ticks, seen = r.t['o'].split(',')
        self.assertLessEqual(int(biggest), 256)
        self.assertEqual(events, 'true')
        self.assertEqual(ticks, '30', 'the timers kept running during the flood')
        self.assertEqual(int(seen), 4096, 'every byte is either delivered or counted as dropped')
        self.assertLessEqual(int(total), 4096)

    def test_partial_write_stuck_and_drained(self):
        r = run(self, PRE + """
const s = serial.open({ mode: 'w' });
const big = new Uint8Array(300).fill(65);
T('a', s.write(big)); T('b', s.write(big)); T('c', s.write('x'));
sleep(10).then(() => { T('d', s.write('é!')); T('st', s.stats().appWritten); sys.exit(0); });
""", ms='200')
        self.assertEqual((r.t['a'], r.t['b'], r.t['c'], r.t['d']), ('256', '0', '0', '3'))
        self.assertEqual(r.serial, b'A' * 256 + 'é!'.encode())
        self.assertEqual(r.t['st'], '259')


class PowerTests(unittest.TestCase):
    def test_holds_tokens_and_the_table(self):
        r = run(self, PRE + """
E('nocaps', () => power.hold('x', { noBlank: false }));
const h = power.hold('keep the music');
T('list', JSON.stringify(power.holds));
const all = [h];
for (let i = 0; i < 15; i++) all.push(power.hold('h' + i, { noSlowdown: true }));
E('full', () => power.hold('one too many'));
h.release(); h.release();
T('after', power.holds.length + ' ' + h.released);
all.forEach((x) => x.release());
T('empty', power.holds.length);
E('again', () => { const k = power.hold('again'); k.release(); return 'fine'; });
sys.exit(0);
""")
        t = r.t
        self.assertIn('EINVAL', t['nocaps'])
        self.assertEqual(json.loads(t['list']), [{'reason': 'keep the music', 'noBlank': True, 'noSlowdown': False}])
        self.assertIn('EMFILE', t['full'])
        self.assertEqual((t['after'], t['empty']), ('15 true', '0'))
        self.assertEqual((r.dev['holds'], r.dev['acquired'], r.dev['released']), (0, 17, 17))

    def test_capability(self):
        r = run(self, PRE + "E('hold', () => power.hold('x')); E('asleep', () => power.asleep); E('on', () => power.on('blank', () => {})); sys.exit(0);", caps=('time',))
        for k in ('hold', 'asleep', 'on'):
            self.assertIn('power', r.t[k], k)

    def test_holds_left_at_stop_are_given_back(self):
        r = run(self, PRE + "power.hold('a'); power.hold('b', { noSlowdown: true }); power.hold('c'); sleep(50).then(() => { T('x', 1); sys.exit(0); });")
        self.assertEqual((r.dev['holds'], r.dev['acquired'], r.dev['released']), (0, 3, 3))
        self.assertTrue(any('left 3 power hold(s)' in l for l in r.rest), r.rest)

    def test_blank_and_wake_events_once_each(self):
        r = run(self, PRE + """
const log = [];
power.on('blank', (e) => log.push('blank@' + Math.round(sandbox.now() / 1000)));
power.on('wake', (e) => log.push('wake@' + Math.round(sandbox.now() / 1000)));
const at = [];
(async () => { for (const ms of [100, 100, 250]) { await sleep(ms); at.push(power.screenDark + ':' + power.asleep.blank); }
  await sleep(100); at.push(power.screenDark + ''); await sleep(300); at.push(power.screenDark + '');
  T('at', at.join(' ')); T('log', log.join(' ')); sys.exit(0); })();
""", ms='1500', args=['--blank', '250:650'])
        self.assertEqual(r.t['at'], 'false:false false:false true:true true false')
        ev = r.t['log'].split()
        self.assertEqual([e.split('@')[0] for e in ev], ['blank', 'wake'])
        self.assertTrue(250 <= int(ev[0].split('@')[1]) <= 400 and 650 <= int(ev[1].split('@')[1]) <= 800, ev)

    def test_old_kernel_is_always_awake(self):
        r = run(self, PRE + "const l = []; power.on('blank', () => l.push('b')); sleep(500).then(() => { T('o', [power.asleep.blank, power.screenDark, l.length].join(',')); sys.exit(0); });", ms='900', args=['--blank', '100:400', '--old-power'])
        self.assertEqual(r.t['o'], 'false,false,0')

    def test_a_hold_keeps_the_screen_lit(self):
        r = run(self, PRE + """
const l = []; power.on('blank', () => l.push('blank')); power.on('wake', () => l.push('wake'));
const h = power.hold('reading');
sleep(300).then(() => { T('held', power.screenDark + ',' + power.asleep.blank); h.release(); return sleep(150); })
  .then(() => { T('free', power.screenDark + ',' + power.asleep.blank + ',' + l.join('.')); sys.exit(0); });
""", ms='900', args=['--blank', '100:1000'])
        self.assertEqual(r.t['held'], 'false,false')
        self.assertEqual(r.t['free'], 'true,true,blank')


class PowerAudio(unittest.TestCase):
    CODE = PRE + """
const calls = [];
const a = audio.open({ onneed(f) { calls.push(Math.round(sandbox.now() / 1000)); a.write(new Int16Array(f * 2).fill(500)); } });
%s
(async () => { await sleep(1900); const s = a.stats();
  const inDark = calls.filter((t) => t > 520 && t < 1190).length;
  T('o', [calls.length > 5, inDark, calls.filter((t) => t > 1400).length > 3, s.darkPolls > 0].join(','));
  sys.exit(0); })();
"""

    def test_the_pump_stops_while_the_screen_is_dark_and_resumes_on_wake(self):
        r = run(self, self.CODE % '', ms='2500', args=['--blank', '400:1200', '--sleepstat'])
        self.assertEqual(r.t['o'], 'true,0,true,true')
        self.assertLessEqual(r.dev['dark_audio_frames'], 4800 * 2, 'only what the app wrote before the pump saw the blank (a look every 100 ms) is dropped')

    def test_tones_wait_in_the_queue_while_dark(self):
        r = run(self, PRE + """
const a = audio.open();
a.tone(440, 1500);
let q;
(async () => { await sleep(300); const s0 = a.stats().pendingTones; await sleep(500); const mid = a.stats();  // dark from 400 to 1100
  await sleep(1700); T('o', [s0, mid.pendingTones, a.stats().pendingTones].join(',')); sys.exit(0); })();
""", ms='3500', args=['--blank', '400:1100'])
        self.assertEqual(r.t['o'], '1,1,0')
        self.assertLessEqual(r.dev['dark_audio_frames'], 4800 * 2)

    def test_a_hold_keeps_audio_going(self):
        r = run(self, self.CODE % "const h = power.hold('music');", ms='2500', args=['--blank', '400:1200'])
        t = r.t['o'].split(',')
        self.assertEqual((t[0], t[2]), ('true', 'true'))
        self.assertGreater(int(t[1]), 10, 'onneed kept being called through the blank window')
        self.assertEqual(t[3], 'false')


class Leaks(unittest.TestCase):
    def test_50_starts_with_everything_open_and_nothing_closed(self):
        d = pathlib.Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, d, True)
        (d / 'OS/APPS/DT').mkdir(parents=True)
        (d / 'OS/APPS/DT/APP.JSON').write_text(json.dumps({'name': 'Dev', 'api': 1, 'capabilities': list(ALLD)}))
        (d / 'OS/APPS/DT/MAIN.JS').write_text("""
globalThis.p = eink.open(); p.canvas().fillRect(0, 0, 9, 9); p.flush();
globalThis.s = serial.open(); s.on('data', () => {}); s.write('hi');
globalThis.h1 = power.hold('a'); globalThis.h2 = power.hold('b', { noSlowdown: true }); power.on('blank', () => {});
globalThis.a = audio.open({ onneed(f) { a.write(new Int16Array(f * 2)); } });
""")
        p = subprocess.run([str(D.RUN), str(d), '/sd/OS/APPS/DT', '--cycles', '50', '--ms', '150', '--vclock', '--devstat'], capture_output=True, text=True, timeout=300)
        m = re.search(r'CYCLES (\d+) blocks first (\d+) last (\d+) leaks (\d+) open (\d+)', p.stdout)
        self.assertTrue(m, p.stdout[-300:] + p.stderr[-300:])
        n, first, last, leaks, opened = map(int, m.groups())
        self.assertEqual((n, opened), (50, 0))
        self.assertEqual(first, last, 'live heap blocks grew')
        dev = dict((k, int(v)) for k, v in re.findall(r'(\w+) (\d+)', re.search(r'HOST dev .*', p.stdout).group(0)[8:]))
        self.assertEqual((dev['eink_open'], dev['serial_open'], dev['audio_open'], dev['holds']), (0, 0, 0, 0))
        self.assertEqual(dev['acquired'], 100)
        self.assertEqual(dev['released'], 100)
        self.assertEqual(dev['serial_opens'], 50)
        self.assertEqual(dev['eink_opens'], 50)
        self.assertEqual(leaks, 50 * 3 + 100, 'eink, serial and audio handles, and the 2 holds, left by each careless start')

    def test_the_devices_are_free_for_the_next_app(self):
        r = run(self, PRE + "eink.open(); serial.open(); power.hold('x'); audio.open(); T('ok', 1); sys.exit(0);")
        self.assertEqual(r.t['ok'], '1')
        self.assertEqual((r.dev['eink_open'], r.dev['serial_open'], r.dev['holds']), (0, 0, 0))


if __name__ == '__main__':
    D.RUN = pathlib.Path(sys.argv.pop(1)).resolve()
    unittest.main()
