#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 4b tests: display (modes, 8 bpp palette round trip, the surface lease and its hand-off with the DOM-lite UI), time
(sandbox.now, sleep in bounded chunks, seeded Math.random) and input (raw key events, repeat, SYN_DROPPED recovery, attach and
detach, bounded events per turn, handle accounting), all on the host. Usage: dev_run.py <jsrun>"""
import json, pathlib, re, shutil, struct, subprocess, sys, tempfile, unittest, zlib

RUN = None
ALL = ('display', 'input')


def go(code, caps=ALL, args=(), ms='400', shot=False, app='DT'):
    """Runs one app on a temporary card; returns (rc, T lines as a dict, other log lines, shot dict or None)."""
    d = pathlib.Path(tempfile.mkdtemp())
    (d / 'OS/APPS' / app).mkdir(parents=True)
    (d / 'OS/APPS' / app / 'APP.JSON').write_text(json.dumps({'name': 'Dev', 'api': 1, 'capabilities': list(caps)}))
    (d / 'OS/APPS' / app / 'MAIN.JS').write_text(code)
    out_shot = d / 'a.shot'
    cmd = [str(RUN), str(d), '/sd/OS/APPS/' + app, '--ms', ms, '--vclock'] + list(args) + (['--shot', str(out_shot)] if shot else [])
    p = subprocess.run(cmd, capture_output=True, text=True, timeout=120)
    lines = [l[3:] if l.startswith('JS ') else l for l in (p.stdout + p.stderr).splitlines()]
    t = {}
    for l in lines:
        if l.startswith('T '):
            k, _, v = l[2:].partition(' ')
            t[k] = v
    sh = None
    if shot and out_shot.exists():
        raw = out_shot.read_bytes()
        w, h, depth = struct.unpack('<4xIII', raw[:16])
        sh = {'w': w, 'h': h, 'depth': depth, 'pal': raw[16:784], 'px': raw[784:], 'crc': zlib.crc32(raw[784:])}
    shutil.rmtree(d, True)
    return p.returncode, t, [l for l in lines if not l.startswith('T ')], sh


PRE = """
const T = (k, v) => console.log('T ' + k + ' ' + v);
const E = (k, fn) => { try { const r = fn(); T(k, 'ok ' + (r === undefined ? '' : r)); } catch (e) { T(k, (e.code || e.name) + ' | ' + e.message); } };
"""


def ok(self, code, **kw):
    rc, t, rest, sh = go(code, **kw)
    self.assertEqual(rc, 0, rest)
    return t, rest, sh


class Display(unittest.TestCase):
    def test_modes_and_the_kernels_rule(self):
        t, _, _ = ok(self, PRE + """
const m = screen.modes();
const has = (w, h, d) => m.some((x) => x.width === w && x.height === h && x.depth === d);
T('has', [has(640, 480, 32), has(960, 540, 16), has(320, 200, 8), has(1280, 720, 8), has(1920, 1080, 8)].join());
T('all', m.every((x) => screen.supports(x.width, x.height, x.depth) && x.pitch === x.width * x.depth / 8 && x.bytes <= 1228800));
T('rule', [screen.supports(641, 480, 32), screen.supports(8, 480, 32), screen.supports(1920, 600, 8), screen.supports(640, 480, 24), screen.supports(1920, 700, 8)].join());
screen.setMode(960, 540, 16); T('old', screen.width + 'x' + screen.height + 'x' + screen.depth);
E('old8', () => screen.setMode(320, 200, 8));
sys.exit(0);
""")
        self.assertEqual(t['has'], 'true,true,true,true,false')
        self.assertEqual(t['all'], 'true')
        self.assertEqual(t['rule'], 'false,false,true,false,false')
        self.assertEqual(t['old'], '960x540x16')
        self.assertIn('RangeError', t['old8'])  # setMode is as it was: 8 bpp is not for pages

    def test_8bpp_lease_palette_round_trip_and_pixels(self):
        t, _, sh = ok(self, PRE + """
const l = screen.lease({ width: 320, height: 200, depth: 8 });
T('shape', [l.width, l.height, l.depth, l.pitch, l.pixels.constructor.name, l.pixels.length, l.pixels.buffer === l.buffer].join());
T('mode', screen.leased + ' page ' + screen.width + 'x' + screen.height + 'x' + screen.depth);
const g = l.getPalette(); T('gray', g[0] + ' ' + g[3 * 100] + ' ' + g[3 * 255 + 2]);
l.setPalette([255, 0, 0, 0, 255, 0, 0, 0, 255, 252, 252, 252], 1);
const p = l.getPalette();
T('pal', [p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11], p[12]].join());
T('quant', [l.setPalette(Uint8Array.of(1, 2, 3, 130, 131, 132), 10), l.getPalette()[30], l.getPalette()[31], l.getPalette()[32], l.getPalette()[33]].join());
E('badlen', () => l.setPalette([1, 2], 0));
E('badrange', () => l.setPalette(new Array(6).fill(1), 255));
E('badval', () => l.setPalette([1, 2, 300], 0));
for (let y = 0; y < 200; y++) l.pixels.fill(y % 4, y * 320, y * 320 + 320);
l.present(0, 0, 320, 200); l.present(); l.present(10, 10, 5, 5);
E('oob', () => l.present(300, 0, 40, 10));
E('empty', () => l.present(0, 0, 0, 10));
""", shot=True)
        self.assertEqual(t['shape'], '320,200,8,320,Uint8Array,64000,true')
        self.assertEqual(t['mode'], 'true page 640x480x32')  # the page's mode is remembered, the lease is another mode
        self.assertEqual(t['gray'], '0 101 255')
        self.assertEqual(t['pal'], '255,0,0,0,255,0,0,0,255,255')  # 252 is 63 in six bits and shows as 255
        self.assertTrue(t['quant'].startswith('2,'), t['quant'])  # two entries set
        self.assertIn('RangeError', t['badlen']); self.assertIn('RangeError', t['badrange']); self.assertIn('RangeError', t['badval'])
        self.assertIn('RangeError', t['oob']); self.assertIn('RangeError', t['empty'])
        self.assertEqual((sh['w'], sh['h'], sh['depth']), (320, 200, 8))
        self.assertEqual(sh['px'][:320 * 4], bytes([0]) * 320 + bytes([1]) * 320 + bytes([2]) * 320 + bytes([3]) * 320)
        # the kernel takes 6-bit values: 255 becomes 63, 130 becomes 32, 252 becomes 63
        self.assertEqual(list(sh['pal'][3:6]), [63, 0, 0])
        self.assertEqual(list(sh['pal'][30:36]), [0, 0, 0, 32, 32, 33])  # entries 10 and 11: (1,2,3) -> 0,0,0 and (130,131,132) -> 32,32,33
        self.assertEqual(list(sh['pal'][9:12]), [0, 0, 63])  # entry 3: blue

    def test_lease_32_and_16_bit_views_and_default_mode(self):
        t, _, sh = ok(self, PRE + """
const a = screen.lease();
T('a', [a.width, a.height, a.depth, a.pixels.constructor.name, a.pixels.length].join());
a.pixels[0] = 0x00ff8040; a.pixels[a.pixels.length - 1] = 0x00123456; a.present(); a.release();
const b = screen.lease({ width: 960, height: 540, depth: 16 });
T('b', [b.width, b.height, b.depth, b.pitch, b.pixels.constructor.name, b.pixels.length].join());
b.pixels[0] = 0xf800; b.pixels[1] = 0x07e0; b.present();
E('badmode', () => { b.release(); return screen.lease({ width: 1920, height: 1080, depth: 32 }); });
""", shot=True)
        self.assertEqual(t['a'], '640,480,32,Uint32Array,307200')
        self.assertEqual(t['b'], '960,540,16,1920,Uint16Array,518400')
        self.assertIn('RangeError', t['badmode'])
        self.assertEqual(sh['w'], 640)  # the failed lease changed nothing: the page's mode is back after b.release()

    def test_lease_and_ui_hand_off_and_refusals(self):
        code = PRE + """
const box = document.createElement('div');
box.style.left = '40px'; box.style.top = '40px'; box.style.width = '200px'; box.style.height = '100px'; box.style.background = '#2060c0';
document.body.appendChild(box);
requestAnimationFrame(() => {
  if (%s) {
    const l = screen.lease({ width: 320, height: 200, depth: 8 });
    E('twice', () => screen.lease());
    E('setmode', () => screen.setMode(800, 600, 16));
    T('leased', screen.leased);
    l.pixels.fill(7); l.present();
    box.style.background = '#c02020'; // the page changes while the app holds the screen: not drawn yet
    setTimeout(() => {
      const view = l.pixels; l.release(); l.release();
      T('after', [screen.leased, l.pixels === view, view.length, view.byteLength, l.pixels.length].join());
      E('stale', () => l.present());
      E('stale2', () => l.setPalette([0, 0, 0]));
      E('again', () => screen.setMode(640, 480, 32));
      box.style.background = '#2060c0';
    }, 50);
  }
});
"""
        _, _, a = ok(self, code % 'false', shot=True)  # the page alone
        t, _, b = ok(self, code % 'true', shot=True)  # lease, release, the same page
        self.assertIn('EBUSY', t['twice'])
        self.assertIn('EBUSY', t['setmode'])
        self.assertEqual(t['leased'], 'true')
        self.assertEqual(t['after'], 'false,true,0,0,0')  # a view kept past release is detached (length 0)
        self.assertIn('EBADF', t['stale']); self.assertIn('EBADF', t['stale2'])
        self.assertIn('ok', t['again'])  # setMode works again
        self.assertEqual((b['w'], b['h'], b['depth']), (640, 480, 32))
        self.assertEqual(a['crc'], b['crc'], 'after release the page paints itself again, pixel for pixel')

    def test_capability_and_stop_while_leased(self):
        t, _, _ = ok(self, PRE + "E('lease', () => screen.lease()); E('modes', () => screen.modes()); sys.exit(0);", caps=('input',))
        self.assertIn("capability 'display'", t['lease']); self.assertIn("capability 'display'", t['modes'])
        rc, _, rest, _ = go(PRE + "globalThis.l = screen.lease({ width: 640, height: 400, depth: 16 }); sys.exit(0);")
        self.assertIn('app left 1 handle(s) open: display 1', ' | '.join(rest))


class Time(unittest.TestCase):
    def test_now_is_exact_microseconds(self):
        t, _, _ = ok(self, PRE + """
const a = sandbox.now(); const b = sandbox.now();
T('int', Number.isInteger(a) && Number.isInteger(b) && b >= a);
sandbox.sleep(25).then(() => { const c = sandbox.now(); T('slept', c - b >= 25000 && c - b < 40000); T('pn', Math.abs((c - a) / 1000 - performance.now()) < 5 + a / 1000); sys.exit(0); });
""")
        self.assertEqual(t['int'], 'true'); self.assertEqual(t['slept'], 'true')

    def test_sleep_order_zero_and_errors(self):
        t, _, _ = ok(self, PRE + """
const order = [];
Promise.all([sleep(30).then(() => order.push('30')), sleep(10).then(() => order.push('10')), sleep(0).then(() => order.push('0')), sandbox.sleep(10).then(() => order.push('10b'))])
  .then(() => { T('order', order.join()); sys.exit(0); });
Promise.resolve().then(() => order.push('micro'));
E('neg', () => sleep(-1)); E('big', () => sleep(1e10)); E('nan', () => sleep(NaN));
""")
        self.assertEqual(t['order'], 'micro,0,10,10b,30')
        for k in ('neg', 'big', 'nan'):
            self.assertIn('RangeError', t[k])

    def test_sleep_never_exceeds_the_heartbeat_chunk(self):
        for extra in ([], ['--keys', '1:0:0:257']):  # with a keyboard (polled every 10 ms) and with none (chunks of 100 ms)
            code = PRE + "sleep(5000).then(() => { T('done', Math.round(performance.now())); sys.exit(0); });"
            rc, t, rest, _ = go(code, args=['--sleepstat'] + extra, ms='6000')
            self.assertEqual(rc, 0, rest)
            self.assertTrue(5000 <= int(t['done']) <= 5200, t)
            m = re.search(r'HOST sleeps (\d+) max_us (\d+)', ' '.join(rest))
            self.assertTrue(m, rest)
            self.assertLessEqual(int(m.group(2)), 100000, 'one kernel sleep longer than the 100 ms chunk')
            self.assertGreater(int(m.group(1)), 40, 'the wait is many bounded sleeps')
            if extra: self.assertGreaterEqual(int(m.group(2)), 90000, 'with no device the loop sleeps the whole chunk')

    def test_random_seeding(self):
        code = PRE + "const r = []; for (let i = 0; i < 6; i++) r.push(Math.random()); T('r', r.join()); T('seed', sandbox.seed); T('range', r.every((x) => x >= 0 && x < 1)); sys.exit(0);"
        a = go(code)[1]; b = go(code)[1]
        self.assertEqual(a['range'], 'true')
        self.assertNotEqual(a['r'], b['r'], 'two runs with no seed must differ')
        self.assertNotEqual(a['seed'], b['seed'])
        c = go(code, args=['--seed', '42'])[1]; d = go(code, args=['--seed', '42'])[1]; e = go(code, args=['--seed', '43'])[1]
        self.assertEqual(c['r'], d['r']); self.assertNotEqual(c['r'], e['r'])
        self.assertEqual(c['seed'], d['seed'])
        again = PRE + "sandbox.seedRandom(7); const r = []; for (let i = 0; i < 6; i++) r.push(Math.random()); T('r', r.join()); sys.exit(0);"
        f = go(again)[1]; g = go(again, args=['--seed', '999'])[1]
        self.assertEqual(f['r'], g['r'], 'seedRandom(n) makes the sequence reproducible whatever the boot seed')
        self.assertNotEqual(f['r'], c['r'])
        vals = [float(x) for x in a['r'].split(',')]
        self.assertEqual(len(set(vals)), 6); self.assertTrue(0.05 < sum(vals) / 6 < 0.95)

    def test_random_mixes_in_key_timing_unless_seeded(self):
        code = PRE + "const q = input.open(); q.on('key', () => {}); setTimeout(() => { T('r', Math.random()); sys.exit(0); }, 300);"
        a = go(code, args=['--seed-live', '5', '--keys', '100:30:1'], ms='600')[1]['r']
        b = go(code, args=['--seed-live', '5', '--keys', '120:30:1'], ms='600')[1]['r']
        c = go(code, args=['--seed-live', '5', '--keys', '120:30:1'], ms='600')[1]['r']
        self.assertNotEqual(a, b); self.assertEqual(b, c)
        fixed = go(code, args=['--seed', '5', '--keys', '100:30:1'], ms='600')[1]['r']
        self.assertEqual(fixed, go(code, args=['--seed', '5', '--keys', '120:30:1'], ms='600')[1]['r'], 'a fixed seed ignores key timings')
        s = PRE + "sandbox.seedRandom(1); const q = input.open(); setTimeout(() => { T('r', Math.random()); sys.exit(0); }, 300);"
        self.assertEqual(go(s, args=['--keys', '100:30:1'], ms='600')[1]['r'], go(s, args=['--keys', '150:31:1,170:31:0'], ms='600')[1]['r'])


class Input(unittest.TestCase):
    LOG = PRE + """
const log = [];
const q = input.open();
q.on('key', (e) => log.push('key ' + e.code + ' ' + e.value + ' ' + e.repeat + ' ' + (e.time > 0)));
q.on('attach', () => log.push('attach')); q.on('detach', () => log.push('detach'));
q.on('reset', (e) => log.push('reset ' + e.reason + ' ' + e.released.join('+')));
document.addEventListener('keydown', (e) => log.push('dom-down ' + e.code + (e.repeat ? ' repeat' : '')));
document.addEventListener('keyup', (e) => log.push('dom-up ' + e.code));
setTimeout(() => { %s log.forEach((x, i) => T('l' + String(i).padStart(3, '0'), x)); sys.exit(0); }, 500);
"""

    @staticmethod
    def L(t):
        return [t[k] for k in sorted(t) if re.fullmatch(r'l\d{3}', k)]

    def test_raw_key_events_and_repeat(self):
        t, _, _ = ok(self, self.LOG % "T('now', q.pressed(103) + ' ' + q.down.join());", args=['--keys', '100:103:1,150:103:2,200:103:2,250:103:0,300:30:1'], ms='700')
        self.assertEqual(self.L(t), ['key 103 1 false true', 'dom-down ArrowUp', 'key 103 2 true true', 'dom-down ArrowUp repeat',
                                     'key 103 2 true true', 'dom-down ArrowUp repeat', 'key 103 0 false true', 'dom-up ArrowUp',
                                     'key 30 1 false true', 'dom-down KeyA'])  # the raw event comes first; the page's events are as before
        self.assertEqual(t['now'], 'false 30')

    def test_syn_dropped_resets_all_keys(self):
        t, _, _ = ok(self, self.LOG % "T('after', q.pressed(103) + ' ' + q.pressed(30) + ' ' + q.down.length);",
                     args=['--keys', '100:103:1,110:30:1,200:3:0:0,300:30:1'], ms='700')
        self.assertEqual(self.L(t)[4:], ['reset dropped 30+103', 'dom-up KeyA', 'dom-up ArrowUp', 'key 30 1 false true', 'dom-down KeyA'])
        self.assertEqual(t['after'], 'false true 1', 'the key pressed again after the reset counts; the earlier ones are released')

    def test_attach_and_detach(self):
        t, _, _ = ok(self, self.LOG % "T('att', input.attached + ' ' + q.attached);",
                     args=['--keys', '50:103:1,100:0:0:257,200:0:0:256,250:28:1'], ms='700')
        self.assertEqual(self.L(t), ['key 103 1 false true', 'dom-down ArrowUp', 'detach', 'reset detached 103', 'dom-up ArrowUp',
                                     'attach', 'key 28 1 false true', 'dom-down Enter'])
        self.assertEqual(t['att'], 'true true')
        t2, _, _ = ok(self, self.LOG % "T('att', input.attached);", args=['--keys', '100:0:0:257'], ms='700')
        self.assertEqual(t2['att'], 'false')
        self.assertEqual(self.L(t2), ['detach', 'reset detached '])

    def test_f10_stays_the_hosts_and_other_types_are_ignored(self):
        rc, t, rest, _ = go(self.LOG % "T('never', 1);", args=['--keys', '50:68:1,60:30:1'], ms='700')
        self.assertEqual(rc, 0, rest); self.assertNotIn('never', t); self.assertEqual(self.L(t), [])  # F10 ended the app before the timer, and no queue saw it

    def test_capability_gate_and_queue_limits(self):
        t, _, _ = ok(self, PRE + "E('open', () => input.open()); sys.exit(0);", caps=('display',))
        self.assertIn("capability 'input'", t['open'])
        t, _, _ = ok(self, PRE + """
const qs = []; for (let i = 0; i < 4; i++) qs.push(input.open());
E('fifth', () => input.open());
qs[0].close(); qs[0].close(); T('closed', qs[0].closed);
E('sixth', () => { const x = input.open(); x.close(); return 'reopened'; });
E('badtype', () => qs[1].on('nope', () => {}));
E('badfn', () => qs[1].on('key', 5));
const f = () => T('called', 'yes'); qs[1].on('key', f).off('key', f);
sys.exit(0);
""")
        self.assertIn('EMFILE', t['fifth']); self.assertEqual(t['closed'], 'true'); self.assertIn('reopened', t['sixth'])
        self.assertIn('TypeError', t['badtype']); self.assertIn('TypeError', t['badfn'])

    def test_a_flood_is_handled_in_bounded_turns(self):
        keys = ','.join('100:%d:%d' % (30 + i % 4, 1 - (i // 4) % 2) for i in range(250))
        t, _, _ = ok(self, PRE + """
let n = 0; const q = input.open(); q.on('key', () => n++);
setTimeout(() => { T('n', n); T('max', sys.stats().inputMaxPerStep); sys.exit(0); }, 400);
""", args=['--keys', keys], ms='700')
        self.assertEqual(t['n'], '250')
        self.assertLessEqual(int(t['max']), 128); self.assertGreater(int(t['max']), 64)

    def test_throwing_listener_does_not_stop_the_others(self):
        t, rest, _ = ok(self, PRE + """
const q = input.open(); q.on('key', () => { throw new Error('boom'); }); q.on('key', (e) => T('second', e.code));
setTimeout(() => sys.exit(0), 300);
""", args=['--keys', '100:30:1'], ms='700')
        self.assertEqual(t['second'], '30')


class LongUptime(unittest.TestCase):
    """Review 2026-11-13: a kernel up more than 71 minutes (2^32 microseconds) still gives key times on sandbox.now()'s clock, and a
    timer too far away to count in nanoseconds does not wake the loop."""

    def test_key_time_is_not_32_bit(self):
        code = PRE + """
const q = input.open();
q.on('key', (e) => { const n = sandbox.now(); T('k', (e.time > 4294967296) + ' ' + (Math.abs(e.time - n) < 100000)); sys.exit(0); });
"""
        rc, t, rest, _ = go(code, caps=('input', 'time'), args=['--keys', '50:30:1', '--vstart-s', '5000'])
        self.assertEqual(rc, 0, rest)
        self.assertEqual(t['k'], 'true true')

    def test_far_timer_does_not_spin(self):
        code = PRE + "setTimeout(() => T('late', 1), 1e15); setTimeout(() => T('soon', 1), 100);\n"
        rc, t, rest, _ = go(code, args=['--vstart-s', '5000', '--soak'], ms='1000')
        self.assertEqual(rc, 0, rest)
        self.assertIn('soon', t); self.assertNotIn('late', t)
        steps = [int(m.group(1)) for l in rest for m in [re.search(r'soak steps (\d+)', l)] if m]
        self.assertTrue(steps and steps[0] < 300, steps)


class Leaks(unittest.TestCase):
    def cycles(self, code, caps=ALL):
        d = pathlib.Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, d, True)
        (d / 'OS/APPS/DT').mkdir(parents=True)
        (d / 'OS/APPS/DT/APP.JSON').write_text(json.dumps({'name': 'Dev', 'api': 1, 'capabilities': list(caps)}))
        (d / 'OS/APPS/DT/MAIN.JS').write_text(code)
        p = subprocess.run([str(RUN), str(d), '/sd/OS/APPS/DT', '--cycles', '50', '--ms', '120', '--vclock', '--keys', '30:30:1,60:3:0:0,80:30:0'], capture_output=True, text=True, timeout=300)
        m = re.search(r'CYCLES (\d+) blocks first (\d+) last (\d+) leaks (\d+) open (\d+)', p.stdout)
        self.assertTrue(m, p.stdout[-300:] + p.stderr[-300:])
        return tuple(map(int, m.groups())), p.stdout

    def test_tidy_app_50_starts_no_leaks(self):
        (n, first, last, leaks, opened), _ = self.cycles("""
const q = input.open(); q.on('key', () => {}); q.on('reset', () => {});
const l = screen.lease({ width: 320, height: 200, depth: 8 }); l.setPalette([1, 2, 3]); l.pixels.fill(1); l.present();
sleep(40).then(() => { l.release(); q.close(); });
""")
        self.assertEqual((n, leaks, opened), (50, 0, 0)); self.assertEqual(first, last, 'live heap blocks grew')

    def test_careless_app_50_starts_everything_released_at_stop(self):
        (n, first, last, leaks, opened), out = self.cycles("""
globalThis.q = input.open(); q.on('key', () => {});
globalThis.l = screen.lease({ width: 640, height: 400, depth: 16 });
sleep(500); sleep(900); // never resolve: the app stops first
""")
        self.assertEqual((n, leaks, opened), (50, 150, 0), 'a queue, a lease and the sleep timer each start; stop releases all three')
        self.assertEqual(first, last, 'live heap blocks grew')
        self.assertIn('left 3 handle(s) open: input 1, timer 1, display 1', out)

    def test_dropped_lease_is_released_when_freed(self):
        t, _, sh = ok(self, PRE + """
(() => { const l = screen.lease({ width: 320, height: 200, depth: 8 }); T('in', screen.leased); })();
T('out', screen.leased); sys.exit(0); // the reference count dropped to zero when the function returned
""", shot=True)
        self.assertEqual(t['in'], 'true')
        self.assertEqual(t['out'], 'false')


if __name__ == '__main__':
    RUN = pathlib.Path(sys.argv.pop(1)).resolve()
    unittest.main()
