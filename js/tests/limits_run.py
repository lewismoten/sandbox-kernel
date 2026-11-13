#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 5 tests: execution limits. A runaway callback (a loop, a microtask chain, a regular expression that backtracks forever),
an app that fills its heap (even one that catches the error), deep recursion and over-nested elements, all on the host with the
real clock (a virtual one never advances inside a loop). A stopped app must: end within the script budget, give the caller
(sandbox.launchError) or the picker the reason, release what it held, leak nothing and never reach the kernel's 2 s heartbeat.
Usage: limits_run.py <jsrun>"""
import json, pathlib, re, shutil, subprocess, sys, tempfile, unittest

RUN = None
BUDGET_US = 500_000    # JSRT_STEP_BUDGET_MS
HEARTBEAT_US = 2_000_000


def manifest(caps):
    return json.dumps({'name': 'T', 'version': '1.0.0', 'api': 1, 'capabilities': list(caps)})


# A caller that launches BAD, then reports what it was told and exits.
CALLER = """
const e = sandbox.launchError;
if (!e) setTimeout(() => apps.launch('BAD'), 30);
else { console.log('T err ' + e.id + ' | ' + e.reason);
  try { const a = audio.open(); console.log('T audio ok'); a.close(); } catch (x) { console.log('T audio ' + x.message); }
  try { const p = eink.open(); console.log('T eink ' + (p ? 'ok' : 'none')); if (p) p.release(); } catch (x) { console.log('T eink ' + x.message); }
  setTimeout(() => sys.exit(0), 30); }
"""
CAPS = ('apps.launch', 'display', 'audio', 'eink', 'input')


def run(bad, caps=CAPS, autorun='CALLER', extra=(), ms=3500, caller=CALLER):
    d = pathlib.Path(tempfile.mkdtemp())
    try:
        for name, code in (('CALLER', caller), ('BAD', bad)):
            (d / 'OS/APPS' / name).mkdir(parents=True)
            (d / 'OS/APPS' / name / 'APP.JSON').write_text(manifest(caps))
            (d / 'OS/APPS' / name / 'MAIN.JS').write_text(code)
        (d / 'OS/AUTORUN.TXT').write_text('JS.ELF\nAPPS/%s\n' % autorun)
        esc = ','.join('%d:1:1' % at for at in range(250, ms, 100))   # the picker quits on Esc, so a run ends when the apps have
        more = [k for k in extra if ':' in k]
        keys = [k for k in extra if k != '--keys' and k not in more]
        p = subprocess.run([str(RUN), str(d), 'x', '--autorun', '--soak', '--ms', str(ms), '--keys', ','.join(more + [esc])] + keys, capture_output=True, text=True, timeout=120)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    lines = [l[3:] if l.startswith('JS ') else l for l in (p.stdout + p.stderr).splitlines()]
    m = re.search(r'HOST soak steps (\d+) max_step_us (\d+) heap_peak (\d+) blocks_start (-?\d+) blocks_end (-?\d+) heap_blocks (\d+)', '\n'.join(lines))
    soak = dict(zip(('steps', 'max_step_us', 'heap_peak', 'b0', 'b1', 'heap_blocks'), map(int, m.groups()))) if m else {}
    return p.returncode, lines, soak


def t(lines):
    return {l[2:].split(' ', 1)[0]: l[2:].split(' ', 1)[1] if ' ' in l[2:] else '' for l in lines if l.startswith('T ')}


class Time(unittest.TestCase):
    def check(self, body, what='a script ran too long', **kw):
        rc, lines, soak = run(body, **kw)
        r = t(lines)
        self.assertIn(what, r.get('err', ''), lines)
        self.assertTrue(r['err'].startswith('BAD | '), r)
        self.assertEqual(rc, 0, lines)
        self.assertLess(soak['max_step_us'], (1_000_000 if 'limit 1000' in r['err'] else BUDGET_US) + 250_000, 'the stop came late')
        self.assertLess(soak['max_step_us'], HEARTBEAT_US - 500_000)   # one loop turn or one start, well under the kernel's 2 s heartbeat
        self.assertEqual(soak['heap_blocks'], 0)
        self.assertEqual([l for l in lines if 'Uncaught' in l or 'left ' in l and 'open' in l and 'audio' not in l and 'eink' not in l], [], lines)
        return r, lines

    def test_a_loop_in_a_timer_callback(self):
        r, lines = self.check("setTimeout(() => { while (true) {} }, 20);")
        self.assertIn('(limit 500 ms)', r['err'])
        self.assertEqual(r['audio'], 'ok')   # what the stopped app held is free again
        self.assertTrue(r['eink'] in ('ok', 'none'))

    def test_the_loop_cannot_be_caught(self):
        r, lines = self.check("setTimeout(() => { try { while (true) {} } catch (e) { console.log('T caught'); } finally { console.log('T finally'); } }, 20);")
        self.assertNotIn('caught', r)

    def test_a_loop_while_loading(self):
        r, lines = self.check("for (;;) {}")
        self.assertIn('(limit 1000 ms)', r['err'])

    def test_a_loop_in_a_frame_callback(self):
        self.check("requestAnimationFrame(function f() { for (let i = 0; ; i++) {} });")

    def test_a_loop_in_a_key_listener(self):
        self.check("document.addEventListener('keydown', () => { for (;;) {} }); setTimeout(() => {}, 10);", extra=['--keys', '300:57:1'])

    def test_an_endless_microtask_chain(self):
        self.check("setTimeout(() => { (function f() { Promise.resolve().then(f); })(); }, 20);")

    def test_a_regular_expression_that_never_ends(self):
        self.check("setTimeout(() => { /(a*)*b/.test('a'.repeat(40)); }, 20);")

    def test_a_native_sort_with_a_slow_comparator(self):
        self.check("setTimeout(() => { const a = Array.from({length: 4000}, (_, i) => i); for (;;) a.sort((x, y) => { for (let i = 0; i < 1000; i++); return y - x; }); }, 20);")

    def test_a_busy_but_honest_app_is_left_alone(self):
        # 20 ms of work a frame for 60 frames is slow, not a runaway
        rc, lines, soak = run("let n = 0; requestAnimationFrame(function f() { const t = sandbox.now(); while (sandbox.now() - t < 20000); if (++n < 30) requestAnimationFrame(f); else sys.exit(0); });", autorun='BAD', ms=3000)
        self.assertEqual([l for l in lines if 'stopped:' in l], [], lines)
        self.assertEqual(rc, 0)


class Memory(unittest.TestCase):
    def check(self, body, **kw):
        rc, lines, soak = run(body, **kw)
        r = t(lines)
        self.assertIn('ran out of memory', r.get('err', ''), lines)
        self.assertIn('limit 1536 KiB', r['err'])
        self.assertEqual(rc, 0, lines)
        self.assertEqual(soak['heap_blocks'], 0, 'the stopped app left its heap behind')
        self.assertLessEqual(soak['heap_peak'], 1536 * 1024 + 192 * 1024)   # the limit, and what one burst may add before the stop
        return r, lines, soak

    def test_filling_the_heap_with_arrays(self):
        self.check("setTimeout(() => { const a = []; for (;;) a.push(new Array(1000).fill(1)); }, 20);")

    def test_big_strings_that_pile_up(self):
        self.check("setTimeout(() => { const a = []; for (let i = 0; ; i++) a.push('x'.repeat(100000) + i); }, 20);")

    def test_one_huge_buffer(self):
        self.check("setTimeout(() => { new ArrayBuffer(64 << 20); console.log('T survived'); }, 20);")

    def test_catching_the_error_does_not_keep_the_app_alive(self):
        r, lines, _ = self.check("setTimeout(() => { const a = []; try { for (;;) a.push(new Array(1000).fill(1)); } catch (e) { console.log('T caught ' + e.name); } console.log('T afterwards'); }, 20);")
        self.assertNotIn('survived', r)

    def test_while_loading(self):
        self.check("const a = []; for (;;) a.push(new Array(1000).fill(1));")

    def test_the_limit_is_the_heap_not_the_array(self):
        rc, lines, soak = run("setTimeout(() => { const a = new Array(100); for (let i = 0; i < 100; i++) a[i] = new Array(500).fill(i); console.log('T ok ' + a.length); sys.exit(0); }, 20);", autorun='BAD')
        self.assertEqual(t(lines).get('ok'), '100', lines)   # about 0.8 MB: inside 1.5 MiB
        self.assertEqual(soak['heap_blocks'], 0)

    def test_a_smaller_limit_given_by_the_host(self):
        rc, lines, soak = run("setTimeout(() => { const a = []; for (let i = 0; i < 100; i++) a.push(new Array(1000).fill(i)); console.log('T ok'); sys.exit(0); }, 20);", extra=['--heap', '700000'])
        self.assertIn('limit 683 KiB', t(lines).get('err', ''), lines)

    def test_a_stopped_app_that_held_the_panel_and_the_speaker(self):
        r, lines, _ = self.check("setTimeout(() => { const a = audio.open(); const p = eink.open(); const q = []; for (;;) q.push(new Array(1000).fill(1)); }, 20);")
        self.assertEqual(r['audio'], 'ok')
        self.assertTrue(any('left 2 handle(s) open' in l for l in lines), lines)


class Stack(unittest.TestCase):
    def go(self, code, caps=('display',)):
        rc, lines, soak = run('', caps=caps, autorun='CALLER', caller="try {\n" + code + "\n} catch (e) { console.log('T outer ' + e.name); }\nsetTimeout(() => sys.exit(0), 20);")
        self.assertEqual(rc, 0, lines)
        self.assertEqual([l for l in lines if 'stopped:' in l], [], lines)
        return t(lines)

    def test_deep_recursion_is_an_error_the_app_can_catch(self):
        r = self.go("function f() { return f() + 1; }\ntry { f(); } catch (e) { console.log('T rec ' + e.name + ' ' + e.message); }\nconsole.log('T after 1');")
        self.assertIn('call stack', r['rec'].lower())
        self.assertEqual(r['after'], '1')

    def test_recursion_through_native_calls(self):
        r = self.go("""const o = {}; o.toString = function () { return String(o); };
try { String(o); } catch (e) { console.log('T a ' + e.name); }
const g = function () { return [1].map(g); };
try { g(); } catch (e) { console.log('T b ' + e.name); }
try { JSON.parse('['.repeat(200000) + ']'.repeat(200000)); } catch (e) { console.log('T c ' + e.name); }
let n = []; for (let i = 0; i < 5000; i++) n = [n];
try { JSON.stringify(n); } catch (e) { console.log('T d ' + e.name); }
try { Math.max.apply(null, new Array(100000)); } catch (e) { console.log('T e ' + e.name); }
try { new RegExp('('.repeat(100000) + ')'.repeat(100000)); } catch (e) { console.log('T f ' + e.name); }
""")
        for k in 'abcdef':
            self.assertIn(k, r, r)

    def test_a_runaway_recursion_with_no_catch_ends_the_callback_not_the_runtime(self):
        rc, lines, soak = run('', autorun='CALLER', caller="setTimeout(() => { (function f() { f(); })(); }, 10); setTimeout(() => { console.log('T still here'); sys.exit(0); }, 100);")
        self.assertEqual(t(lines).get('still'), 'here', lines)
        self.assertTrue(any('call stack' in l.lower() for l in lines), lines)

    def test_elements_nest_to_a_limit(self):
        r = self.go("""let p = document.body, n = 0;
try { for (; n < 1000; n++) { const d = document.createElement('div'); p.appendChild(d); p = d; } } catch (e) { console.log('T err ' + e.name + ' | ' + e.message); }
console.log('T depth ' + n);
// a deep chain built bottom-up is refused the same way
let top = document.createElement('div'), m = 0;
try { for (; m < 1000; m++) { const d = document.createElement('div'); d.appendChild(top); top = d; } } catch (e) { console.log('T up ' + e.name); }
console.log('T up_depth ' + m);
""")
        self.assertEqual(r['err'].split(' | ')[0], 'RangeError')
        self.assertIn('64', r['err'])
        self.assertEqual(int(r['depth']), 64)   # body is level 0
        self.assertEqual(r['up'], 'RangeError')
        self.assertEqual(int(r['up_depth']), 64)


class Picker(unittest.TestCase):
    def test_the_only_app_runs_away_and_the_picker_says_why(self):
        rc, lines, soak = run("setTimeout(() => { for (;;) {} }, 20);", autorun='BAD')
        self.assertTrue(any('stopped: a script ran too long' in l for l in lines), lines)
        self.assertEqual(len([l for l in lines if 'runtime ready' in l]), 2, lines)   # the app, then the picker
        self.assertLess(soak['max_step_us'], BUDGET_US + 250_000)
        self.assertEqual(soak['heap_blocks'], 0)


if __name__ == '__main__':
    RUN = pathlib.Path(sys.argv.pop(1)).resolve()
    unittest.main(verbosity=1)
