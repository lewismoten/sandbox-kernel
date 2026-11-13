#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 3 tests: the app manifest (C validator and tools/app_check.py agree), capabilities, the path policy, the handle
table (stale handles, exhaustion, leaks at stop, 50 app switches), the test profile's hooks under the policy, and AUTORUN
with its fall back to the picker. Usage: app_run.py <jsrun> <jsrun-pt> <appcheck>"""
import json, pathlib, re, shutil, subprocess, sys, tempfile, unittest

JS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(JS / 'tools'))
import app_check
RUN = PT = CHK = None


def card(apps=None, autorun=None, files=None):
    """A temporary card: apps = {NAME: {file: text}}, files = {'GAMES/G1/GAME.GAM': text}."""
    d = pathlib.Path(tempfile.mkdtemp())
    (d / 'OS/APPS').mkdir(parents=True)
    for name, fl in (apps or {}).items():
        (d / 'OS/APPS' / name).mkdir()
        for f, t in fl.items():
            (d / 'OS/APPS' / name / f).write_text(t)
    if autorun is not None:
        (d / 'OS/AUTORUN.TXT').write_text(autorun)
    for f, t in (files or {}).items():
        (d / f).parent.mkdir(parents=True, exist_ok=True)
        (d / f).write_text(t)
    return d


def run(exe, apps, args, autorun=None, files=None, app=None):
    d = card(apps, autorun, files)
    try:
        a = [str(exe), str(d), app or '/sd/OS/APPS/' + (next(iter(apps)) if apps else 'X')] + args + ['--vclock']
        p = subprocess.run(a, capture_output=True, text=True, timeout=120)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    out = (p.stdout + p.stderr).splitlines()
    return p.returncode, [l[3:] if l.startswith('JS ') else l for l in out]


def manifest(caps=(), **kw):
    m = {'name': 'Test', 'version': '1.0.0', 'api': 1, 'capabilities': list(caps)}
    m.update(kw)
    return json.dumps(m)


def tlines(out):
    return [l[2:] for l in out if l.startswith('T ')]


class Manifest(unittest.TestCase):
    OK = {
        'minimal': '{"name":"A","api":1}',
        'full': '{"schema":1,"name":"Clock","version":"1.2.3","entry":"main.js","api":1,"description":"d","capabilities":["fs.read","display","input","audio","eink","serial","power","fs.write.saves","fs.write.cache","fs.read.shared","log"],"x-note":{"a":[1,2,{"b":null}]}}',
        'spaces': ' \n{ "name" : "A" ,\t"api" : 1 }\n',
        'entry8': '{"name":"A","api":1,"entry":"ABCD_E-1.js"}',
        'empty caps': '{"name":"A","api":1,"capabilities":[]}',
        'escapes': '{"name":"a\\"b\\\\c\\/d","api":1}',
    }
    BAD = {  # name: (text, a word the message holds)
        'empty': ('', 'JSON object'), 'not json': ('hello', 'JSON object'), 'array': ('[]', 'JSON object'),
        'truncated': ('{"name":"A","api":1', 'expected'), 'trailing comma': ('{"name":"A","api":1,}', 'string'),
        'trailing text': ('{"name":"A","api":1} x', 'after'), 'single quotes': ("{'name':'A','api':1}", 'string'),
        'missing name': ('{"api":1}', 'name'), 'missing api': ('{"name":"A"}', 'api'),
        'empty name': ('{"name":"","api":1}', 'name'), 'long name': ('{"name":"%s","api":1}' % ('x' * 40), 'too long'),
        'name number': ('{"name":5,"api":1}', 'name'), 'api string': ('{"name":"A","api":"1"}', 'api'),
        'api zero': ('{"name":"A","api":0}', 'api'), 'api float': ('{"name":"A","api":1.5}', 'api'),
        'api negative': ('{"name":"A","api":-1}', 'api'), 'api leading zero': ('{"name":"A","api":01}', 'zero'),
        'api huge': ('{"name":"A","api":70000}', 'large'), 'api true': ('{"name":"A","api":true}', 'api'),
        'newer api': ('{"name":"A","api":2}', 'API'),  # the validator alone does not know the runtime's; see test_newer_api
        'unknown cap': ('{"name":"A","api":1,"capabilities":["wifi"]}', 'wifi'),
        'caps not list': ('{"name":"A","api":1,"capabilities":"input"}', 'list'),
        'cap not string': ('{"name":"A","api":1,"capabilities":[1]}', 'capability'),
        'cap twice': ('{"name":"A","api":1,"capabilities":["input","input"]}', 'twice'),
        'unknown key': ('{"name":"A","api":1,"capabilites":[]}', 'capabilites'),
        'duplicate key': ('{"name":"A","name":"B","api":1}', 'twice'),
        'bad entry path': ('{"name":"A","api":1,"entry":"../X.JS"}', 'entry'),
        'bad entry ext': ('{"name":"A","api":1,"entry":"MAIN.TXT"}', 'entry'),
        'entry long': ('{"name":"A","api":1,"entry":"ABCDEFGHI.JS"}', 'entry'),
        'entry dir': ('{"name":"A","api":1,"entry":"A/B.JS"}', 'entry'),
        'bad version': ('{"name":"A","api":1,"version":"one"}', 'version'),
        'version 4 parts': ('{"name":"A","api":1,"version":"1.2.3.4"}', 'version'),
        'version number': ('{"name":"A","api":1,"version":1}', 'version'),
        'schema 2': ('{"name":"A","api":1,"schema":2}', 'schema'),
        'control char': ('{"name":"a\\u0001","api":1}', 'escape'), 'raw control': ('{"name":"a\x01","api":1}', 'control'),
        'non ascii': ('{"name":"caf\u00e9","api":1}', 'ASCII'), 'bom': ('\ufeff{"name":"A","api":1}', 'JSON object'),
        'oversize': ('{"name":"A","api":1,"x-pad":"%s"}' % ('p' * 2100), '2048'),
        'deep': ('{"name":"A","api":1,"x-d":[[[[[[[[1]]]]]]]]}', 'deeply'),
        'nan': ('{"name":"A","api":1,"x-n":NaN}', 'JSON'),
        'description long': ('{"name":"A","api":1,"description":"%s"}' % ('d' * 130), 'description'),
    }

    def c(self, text):
        f = pathlib.Path(tempfile.mkstemp()[1])
        f.write_bytes(text.encode('utf-8'))
        try:
            return subprocess.run([CHK, 'manifest', str(f)], capture_output=True, text=True).stdout.strip()
        finally:
            f.unlink()

    def test_valid_manifests_agree_with_the_python_checker(self):
        for name, text in self.OK.items():
            with self.subTest(name):
                c = self.c(text)
                self.assertTrue(c.startswith('ok '), c)
                self.assertEqual(c, app_check.line(app_check.check_manifest(text.encode(), api=1)))

    def test_malformed_manifests_are_refused_by_both(self):
        for name, (text, word) in self.BAD.items():
            with self.subTest(name):
                c = self.c(text)
                py = app_check.line(app_check.check_manifest(text.encode('utf-8'), api=1 if name != 'newer api' else 2 - 1))
                if name == 'newer api':  # the C validator compares only the form; the runtime start compares the number
                    self.assertTrue(c.startswith('ok '), c)
                    self.assertTrue(py.startswith('err ') and 'API' in py, py)
                    continue
                self.assertTrue(c.startswith('err '), c)
                self.assertTrue(py.startswith('err '), py)
                self.assertIn(word.lower(), c.lower())

    def test_c_and_python_agree_on_an_edit_fuzz(self):
        base = self.OK['full']
        import random
        rnd = random.Random(7)
        for _ in range(300):
            t = list(base)
            for _ in range(rnd.randint(1, 3)):
                i = rnd.randrange(len(t))
                t[i] = rnd.choice(['"', ',', '{', '}', '[', ']', ':', '1', 'x', ' ', '\\']) if rnd.random() < .7 else ''
            text = ''.join(t)
            c = self.c(text)
            py = app_check.line(app_check.check_manifest(text.encode(), api=1))
            self.assertEqual(c.startswith('ok '), py.startswith('ok '), f'{text!r}\n C: {c}\n py: {py}')
            if c.startswith('ok '):
                self.assertEqual(c, py)


class Paths(unittest.TestCase):
    def path(self, caps, mode, p, id='PATHS'):
        return subprocess.run([CHK, 'path', id, caps, mode, p], capture_output=True, text=True).stdout.strip()

    def test_reads(self):
        ok = ['/sd/OS/APPS/PATHS/MAIN.JS', '/sd/os/apps/paths/main.js', '/sd/OS/APPS/PATHS', '/sd/OS/APPS/PATHS/SUB/A.TXT', '/sd/OS/SAVES/PATHS/S1.SAV']
        no = {
            '/sd/OS/APPS/OTHER/MAIN.JS': 'another app', '/sd/OS/APPS/PATHSX/MAIN.JS': 'another app', '/sd/OS/APPS/PATHS/../OTHER/MAIN.JS': "'..'",
            '/sd/OS/APPS/PATHS/./MAIN.JS': "'.'", '/sd/OS/APPS/PATHS//MAIN.JS': 'empty', '/sd/OS/APPS/PATHS/': 'empty', '/sd/OS/APPS/PATHS/MAIN.JS.': 'dot or space',
            '/sd/OS/APPS/PATHS /MAIN.JS': 'dot or space', '/sd/../etc/passwd': "'..'", '/dev/log': 'device', '/dev/input/event0': 'device', 'MAIN.JS': '/sd/',
            '': '/sd/', '/sd': '/sd/', '/sd/OS/AUTORUN.TXT': 'outside', '/sd/OS/SAVES/OTHER/X.SAV': 'outside', '/sd/OS/APPS': 'another app',
            '/sd/GAMES/G1/GAME.GAM': 'fs.read.shared', '/sd/OS/APPS/PATHS~1/MAIN.JS': 'character', '/sd/OS/APPS/PATHS\\..\\OTHER': 'character',
            '/sd/OS/CACHE/PATHS': 'outside', '/sd/C:/X': 'character', '/sd/OS/APPS/PATHS/A*.JS': 'character', '/sd/' + 'a/' * 50: 'too long',
        }
        for p in ok:
            with self.subTest(p):
                self.assertEqual(self.path('fs.read', 'r', p), 'allow')
        for p, why in no.items():
            with self.subTest(p):
                r = self.path('fs.read', 'r', p)
                self.assertTrue(r.startswith('deny'), r)
                self.assertIn(why.lower(), r.lower())
        self.assertTrue(self.path('none', 'r', '/sd/OS/APPS/PATHS/MAIN.JS').startswith('deny'), 'no fs.read, no reads')

    def test_shared_reads_stop_at_the_os_folder(self):
        self.assertEqual(self.path('fs.read,fs.read.shared', 'r', '/sd/GAMES/G1/GAME.GAM'), 'allow')
        for p in ('/sd/OS/APPS/OTHER/MAIN.JS', '/sd/OS/AUTORUN.TXT', '/sd/OS/SAVES/OTHER/X', '/sd/GAMES/../OS/AUTORUN.TXT', '/sd/os'):
            self.assertTrue(self.path('fs.read,fs.read.shared', 'r', p).startswith('deny'), p)

    def test_writes(self):
        w = lambda caps, p: self.path(caps, 'w', p)
        self.assertEqual(w('fs.write.saves', '/sd/OS/SAVES/PATHS/S1.SAV'), 'allow')
        self.assertEqual(w('fs.write.saves', '/sd/os/saves/paths/s1.sav'), 'allow')
        self.assertTrue(w('fs.read', '/sd/OS/SAVES/PATHS/S1.SAV').startswith('deny'), 'write needs fs.write.saves')
        self.assertTrue(w('fs.write.saves', '/sd/OS/SAVES/PATHS').startswith('deny'), 'the folder itself')
        self.assertTrue(w('fs.write.saves', '/sd/OS/SAVES/OTHER/S1.SAV').startswith('deny'))
        self.assertTrue(w('fs.write.saves', '/sd/OS/SAVES/PATHS/../OTHER/S1').startswith('deny'))
        self.assertIn('read-only', w('fs.write.saves,fs.read,fs.write.cache', '/sd/OS/APPS/PATHS/MAIN.JS'))
        self.assertTrue(w('fs.write.saves', '/sd/GAMES/G1/GAME.GAM').startswith('deny'))
        self.assertTrue(w('fs.write.saves,fs.read.shared', '/sd/GAMES/G1/GAME.GAM').startswith('deny'), 'shared is read only')
        self.assertEqual(w('fs.write.cache', '/sd/OS/CACHE/PATHS'), 'allow')
        self.assertTrue(w('fs.write.cache', '/sd/OS/CACHE/PATHS/X').startswith('deny'))
        self.assertTrue(w('fs.write.cache', '/sd/OS/CACHE/OTHER').startswith('deny'))
        self.assertTrue(w('fs.write.saves', '/dev/fb0').startswith('deny'))


DENY = r"""
const t = (n, f) => { try { f(); console.log('T ' + n + ' ALLOWED'); } catch (e) { console.log('T ' + n + ' ' + e.message); } };
t('readFile', () => fs.readFile('/sd/OS/APPS/DENY/MAIN.JS'));
t('readdir', () => fs.readdir('/sd/OS/APPS/DENY'));
t('open', () => fs.open('/sd/OS/APPS/DENY/MAIN.JS'));
t('setMode', () => screen.setMode(640, 480, 32));
t('gameLowres', () => screen.gameLowres(true));
t('createElement', () => document.createElement('div'));
t('raf', () => requestAnimationFrame(() => {}));
for (const c of ['fs.read', 'fs.read.shared', 'fs.write.saves', 'fs.write.cache', 'display', 'input', 'audio', 'eink', 'serial', 'power', 'log', 'time', 'memory', 'app.lifecycle'])
  t('require ' + c, () => sandbox.require(c));
t('require wifi', () => sandbox.require('wifi'));
console.log('T caps ' + JSON.stringify(sandbox.capabilities) + ' ' + JSON.stringify(sandbox.version) + ' ' + sandbox.has('input') + ' ' + sandbox.has('log'));
console.log('T app ' + JSON.stringify(sandbox.app));
let keys = 0;
document.addEventListener('keydown', () => keys++);
setTimeout(() => console.log('T keys ' + keys), 400);
setTimeout(() => sys.exit(0), 450);
"""


class Capabilities(unittest.TestCase):
    def go(self, caps, extra='', code=DENY, keys='100:30:1,110:30:0', **kw):
        rc, out = run(RUN, {'DENY': {'APP.JSON': manifest(caps, **kw), 'MAIN.JS': code}}, ['--ms', '600', '--keys', keys])
        self.assertEqual(rc, 0, out)
        return {(' '.join(l.split(' ')[:2]) if l.startswith('require') else l.split(' ', 1)[0]): l for l in tlines(out)}, out

    def test_nothing_granted_names_each_missing_capability(self):
        t, out = self.go([])
        for call, cap in (('readFile', 'fs.read'), ('readdir', 'fs.read'), ('open', 'fs.read'), ('setMode', 'display'), ('gameLowres', 'display'),
                          ('createElement', 'display'), ('raf', 'display')):
            self.assertRegex(t[call], f"capability '{re.escape(cap)}' is not granted to this app", call)
            self.assertNotIn('ALLOWED', t[call])
        for cap in ('fs.read', 'fs.read.shared', 'fs.write.saves', 'fs.write.cache', 'display', 'input', 'audio', 'eink', 'serial', 'power'):
            self.assertIn(f"capability '{cap}' is not granted", t['require ' + cap], cap)
        for cap in ('log', 'time', 'memory', 'app.lifecycle'):
            self.assertIn('ALLOWED', t['require ' + cap], cap + ' is always granted')
        self.assertIn('unknown capability', t['require wifi'])
        self.assertIn('["log","time","memory","app.lifecycle"] {"api":1,"runtime":"%s"} false true' % (pathlib.Path(__file__).resolve().parents[1] / 'VERSION').read_text().strip(), t['caps'])
        self.assertIn('{"id":"DENY","name":"Test","version":"1.0.0","legacy":false}', t['app'])
        self.assertEqual(t['keys'], 'keys 0', 'no input capability: no key events')

    def test_each_grant_opens_only_its_calls(self):
        t, _ = self.go(['fs.read'])
        self.assertIn('ALLOWED', t['readFile']); self.assertIn('ALLOWED', t['readdir']); self.assertIn('ALLOWED', t['open'])
        self.assertIn("'display'", t['setMode'])
        t, _ = self.go(['display', 'input'])
        self.assertIn("'fs.read'", t['readFile'])
        for c in ('setMode', 'gameLowres', 'createElement', 'raf'):
            self.assertIn('ALLOWED', t[c], c)
        self.assertEqual(t['keys'], 'keys 1')
        self.assertIn('ALLOWED', t['require input']); self.assertIn("'audio'", t['require audio'])
        t, _ = self.go(['fs.write.saves'])
        self.assertIn('path refused', t['readFile'], 'saves access passes the capability, and the policy keeps the app out of its own folder')

    def test_manifest_name_and_entry(self):
        rc, out = run(RUN, {'ENT': {'APP.JSON': manifest(entry='start.js'), 'START.JS': "console.log('T entry ran');"}}, ['--ms', '50'])
        self.assertEqual(rc, 0, out)
        self.assertIn('entry ran', tlines(out)[0])
        self.assertTrue(any(l.startswith('app Test 1.0.0 api 1: no extra') for l in out), out)

    def test_legacy_app_without_a_manifest_keeps_every_capability(self):
        rc, out = run(RUN, {'OLD': {'MAIN.JS': DENY.replace('DENY', 'OLD')}}, ['--ms', '600', '--keys', '100:30:1,110:30:0'])
        self.assertEqual(rc, 0, out)
        self.assertEqual([l for l in tlines(out) if 'is not granted' in l], [])
        self.assertTrue(any('"legacy":true' in l for l in tlines(out)))
        rc, out = run(RUN, {'OLD': {'MAIN.JS': 'console.log("T x")'}}, ['--ms', '50', '--strict'])
        self.assertEqual(rc, 1)
        self.assertTrue(any('refused: APP.JSON is missing' in l for l in out), out)


PATHS = r"""
const bad = [
  '/sd/OS/APPS/OTHER/MAIN.JS', '/sd/OS/APPS/PATHS/../OTHER/MAIN.JS', '/sd/OS/APPS/PATHS/./MAIN.JS', '/sd/OS/APPS/PATHS//MAIN.JS',
  '/sd/OS/APPS/PATHS/MAIN.JS.', '/sd/OS/APPS/PATHS /MAIN.JS', '/sd/../etc/passwd', '/dev/log', '/dev/input/event0', 'MAIN.JS', '',
  '/sd/OS/AUTORUN.TXT', '/sd/OS/SAVES/OTHER/X.SAV', '/sd/OS/APPS', '/sd/GAMES/G1/GAME.GAM', '/sd/OS/APPS/PATHS~1/MAIN.JS',
  '/sd/OS/APPS/PATHS\\..\\OTHER', '/sd/OS/APPS/PATHS/MAIN.JS\0../../OTHER/MAIN.JS', '/sd/OS/CACHE/PATHS',
];
const good = ['/sd/OS/APPS/PATHS/MAIN.JS', '/sd/os/apps/paths/main.js', '/sd/OS/SAVES/PATHS/S1.SAV'];
for (const p of bad) for (const [n, f] of [['readFile', fs.readFile], ['open', fs.open], ['readdir', fs.readdir]]) {
  try { f(p); console.log('T LEAK ' + n + ' ' + JSON.stringify(p)); } catch (e) { if (!/refused|NUL/.test(e.message)) console.log('T NOREASON ' + n + ' ' + JSON.stringify(p) + ' ' + e.message); }
}
for (const p of good) { try { fs.readFile(p); console.log('T ok ' + p); } catch (e) { console.log('T WRONG ' + p + ' ' + e.message); } }
try { console.log('T dir ' + fs.readdir('/sd/OS/APPS/PATHS').length); } catch (e) { console.log('T WRONG readdir ' + e.message); }
try { fs.readFile('/sd/OS/APPS/OTHER/MAIN.JS'); } catch (e) { console.log('T msg ' + e.message); }
"""


class PathPolicy(unittest.TestCase):
    def test_traversal_and_escapes_from_javascript(self):
        rc, out = run(RUN, {'PATHS': {'APP.JSON': manifest(['fs.read']), 'MAIN.JS': PATHS}, 'OTHER': {'MAIN.JS': 'secret'}}, ['--ms', '50'],
                      files={'OS/SAVES/PATHS/S1.SAV': 'save', 'OS/SAVES/OTHER/X.SAV': 'x', 'OS/AUTORUN.TXT': 'JS.ELF\nAPPS/PATHS\n', 'GAMES/G1/GAME.GAM': 'g'})
        self.assertEqual(rc, 0, out)
        t = tlines(out)
        self.assertEqual([l for l in t if l.startswith(('LEAK', 'NOREASON', 'WRONG'))], [], out)
        self.assertEqual([l for l in t if l.startswith('ok')], ['ok /sd/OS/APPS/PATHS/MAIN.JS', 'ok /sd/os/apps/paths/main.js', 'ok /sd/OS/SAVES/PATHS/S1.SAV'])
        self.assertIn('msg fs.readFile: path refused, another app\'s folder is not readable: /sd/OS/APPS/OTHER/MAIN.JS', t)

    def test_shared_grant_reads_games_but_not_the_os_folder(self):
        code = r"""for (const p of ['/sd/GAMES/G1/GAME.GAM', '/sd/OS/AUTORUN.TXT', '/sd/OS/APPS/SH/MAIN.JS', '/sd/GAMES/../OS/AUTORUN.TXT'])
          { try { console.log('T ' + fs.readFile(p).length + ' ' + p); } catch (e) { console.log('T denied ' + p); } }"""
        rc, out = run(RUN, {'SH': {'APP.JSON': manifest(['fs.read.shared']), 'MAIN.JS': code}}, ['--ms', '50'],
                      files={'GAMES/G1/GAME.GAM': 'game!', 'OS/AUTORUN.TXT': 'x\nAPPS/SH\n'})
        t = tlines(out)
        self.assertEqual(t[0], '5 /sd/GAMES/G1/GAME.GAM')
        self.assertEqual(t[1:], ['denied /sd/OS/AUTORUN.TXT', 'denied /sd/OS/APPS/SH/MAIN.JS', 'denied /sd/GAMES/../OS/AUTORUN.TXT'])


HANDLES = r"""
const t = (n, f) => { try { const r = f(); console.log('T ' + n + ' ok ' + (r === undefined ? '' : r)); } catch (e) { console.log('T ' + n + ' ' + e.message); } };
const P = '/sd/OS/APPS/HND/MAIN.JS';
const a = fs.open(P);
t('size', () => a.size > 10);
t('text', () => a.text().startsWith('const t'));
t('close', () => a.close());
t('stale text', () => a.text());
t('stale size', () => a.size);
t('double close', () => a.close());
const b = fs.open(P);  // takes the freed slot with a new generation
t('old after reuse', () => a.text());
t('new works', () => b.text().length > 0);
b.close();
const hs = [];
t('fill', () => { for (let i = 0; i < 12; i++) hs.push(fs.open(P)); return hs.length; });
t('13th', () => fs.open(P));
hs[0].close();
t('after one closed', () => { hs[0] = fs.open(P); return 'reopened'; });
for (const h of hs.slice(0, 9)) h.close();
console.log('T left open: 3');
sys.exit(0);
"""


class Handles(unittest.TestCase):
    def go(self, code, args=('--ms', '100')):
        return run(RUN, {'HND': {'APP.JSON': manifest(['fs.read']), 'MAIN.JS': code}}, list(args))

    def test_stale_and_exhausted_handles(self):
        rc, out = self.go(HANDLES)
        t = tlines(out)
        self.assertEqual(rc, 0, out)
        stale = 'EBADF: the file handle is closed or stale'
        for n in ('stale text', 'stale size', 'double close', 'old after reuse'):
            self.assertIn(f'{n} {stale}', t, n)
        self.assertIn('new works ok true', t)
        self.assertIn('fill ok 12', t)
        self.assertIn('13th fs.open: too many open handles (12); close one first', t)
        self.assertIn('after one closed ok reopened', t)

    def test_stop_reports_and_releases_handles_the_app_left_open(self):
        rc, out = self.go(HANDLES)
        self.assertIn('app left 3 handle(s) open: file 3', out)

    def test_a_well_behaved_app_leaves_nothing(self):
        code = "const f = fs.open('/sd/OS/APPS/HND/MAIN.JS'); f.text(); f.close(); console.log('T done');"
        rc, out = self.go(code)
        self.assertFalse(any('left' in l for l in out), out)

    def test_a_dropped_file_handle_is_closed_by_its_finalizer(self):
        code = "for (let i = 0; i < 40; i++) { fs.open('/sd/OS/APPS/HND/MAIN.JS'); } console.log('T 40 opens, none kept');"
        rc, out = self.go(code)
        self.assertEqual(tlines(out), ['40 opens, none kept'])
        self.assertFalse(any('left' in l for l in out), out)

    def cycles(self, good, other, n=50):
        d = card({'GOOD': good, 'OTHER': other})
        try:
            p = subprocess.run([str(RUN), str(d), '/sd/OS/APPS/GOOD', '--alt', '/sd/OS/APPS/OTHER', '--cycles', str(n), '--ms', '120', '--vclock'], capture_output=True, text=True, timeout=300)
        finally:
            shutil.rmtree(d, ignore_errors=True)
        m = re.search(r'CYCLES (\d+) blocks first (\d+) last (\d+) leaks (\d+) open (\d+)', p.stdout)
        self.assertTrue(m, p.stdout[-300:] + p.stderr[-300:])
        return tuple(int(x) for x in m.groups())

    def test_fifty_app_switches_with_no_leaks(self):
        a = {'APP.JSON': manifest(['fs.read', 'display', 'input']), 'MAIN.JS': "const f = fs.open('/sd/OS/APPS/GOOD/MAIN.JS'); document.body.textContent = f.text().length + ''; f.close(); setTimeout(() => {}, 10);"}
        b = {'APP.JSON': manifest(['display'], name='Other'), 'MAIN.JS': "const e = document.createElement('div'); e.textContent = 'x'; document.body.appendChild(e); setInterval(() => {}, 20);"}
        n, first, last, leaks, opened = self.cycles(a, b)
        self.assertEqual((n, leaks, opened), (50, 0, 0))
        self.assertEqual(first, last, 'live heap blocks grew over 50 app switches')

    def test_fifty_switches_of_a_leaking_app_still_release_everything(self):
        a = {'APP.JSON': manifest(['fs.read']), 'MAIN.JS': "globalThis.keep = [fs.open('/sd/OS/APPS/GOOD/MAIN.JS'), fs.open('/sd/OS/APPS/GOOD/MAIN.JS')];"}
        b = {'APP.JSON': manifest([], name='Other'), 'MAIN.JS': 'void 0;'}
        n, first, last, leaks, opened = self.cycles(a, b)
        self.assertEqual((n, leaks, opened), (50, 50, 0), 'each of the 25 leaking starts left 2 handles')
        self.assertEqual(first, last)


PROFAPP = r"""
console.log('APP start');
let g = document.createElement('game');
g.src = '/sd/GAMES/T1';
g.play();
console.log('APP playing ' + g.playing);
g = null;                       // nobody holds the playing element: the finalizer must end() it
console.log('APP dropped');
setTimeout(() => { console.log('APP later'); sys.exit(0); }, 100);
"""


class ProfileUnderPolicy(unittest.TestCase):
    def pt(self, apps, args=('--ms', '400'), **kw):
        d = card(apps, files=kw.get('files'))
        try:
            p = subprocess.run([str(PT), str(d), '/sd/OS/APPS/' + next(iter(apps))] + list(args) + ['--vclock'], capture_output=True, text=True, timeout=60)
        finally:
            shutil.rmtree(d, ignore_errors=True)
        return p.returncode, [l[3:] if l.startswith('JS ') else l for l in (p.stdout + p.stderr).splitlines() if l]

    def test_a_garbage_collected_playing_element_gets_end(self):
        rc, out = self.pt({'PA': {'MAIN.JS': PROFAPP}})
        t = [l for l in out if l.startswith(('PT ', 'APP '))]
        want = ['PT init', 'APP start', 'PT play /sd/GAMES/T1', 'APP playing true', 'PT end', 'APP dropped', 'APP later']
        self.assertEqual([l for l in t if not l.startswith(('PT draw', 'PT picture'))][:len(want)], want, out)
        self.assertEqual(sum(l == 'PT end' for l in t), 1, 'end() once, not again at stop')
        self.assertTrue(t[-1].startswith('PT stop open=0 allocs=1 frees=1'), t[-1])
        self.assertEqual(rc, 0)

    def test_profile_session_left_open_is_reported_by_the_core(self):
        code = "const g = document.createElement('game'); g.src = '/sd/GAMES/LEAKY'; g.play();"
        rc, out = self.pt({'LK': {'MAIN.JS': code}})
        self.assertIn('PT stop open=1 allocs=1 frees=0', out)
        self.assertIn('app left 1 handle(s) open: game 1', out)
        d = card({'LK': {'MAIN.JS': code}})
        try:
            p = subprocess.run([str(PT), str(d), '/sd/OS/APPS/LK', '--cycles', '50', '--ms', '100', '--vclock'], capture_output=True, text=True, timeout=120)
        finally:
            shutil.rmtree(d, ignore_errors=True)
        m = re.search(r'CYCLES 50 blocks first (\d+) last (\d+) leaks (\d+) open (\d+)', p.stdout)
        self.assertTrue(m, p.stdout[-300:])
        self.assertEqual((m.group(1) == m.group(2), m.group(3), m.group(4)), (True, '50', '0'))

    def test_profile_hooks_read_through_the_path_policy(self):
        code = "const g = document.createElement('game'); g.src = '/sd/GAMES/READ'; try { g.play(); console.log('APP played'); } catch (e) { console.log('APP ' + e.message); }"
        files = {'GAMES/READ/GAME.GAM': 'gamefile', 'OS/AUTORUN.TXT': 'JS.ELF\nAPPS/PP\n'}
        rc, out = self.pt({'PP': {'APP.JSON': manifest(['display', 'fs.read']), 'MAIN.JS': code}}, files=files)
        self.assertIn('APP game.play: path refused, outside the app\'s own folders (needs fs.read.shared): /sd/GAMES/READ', out)
        self.assertFalse(any(l.startswith('PT play') for l in out))
        rc, out = self.pt({'PP': {'APP.JSON': manifest(['display', 'fs.read', 'fs.read.shared']), 'MAIN.JS': code}}, files=files)
        self.assertIn('PT read /sd/GAMES/READ/GAME.GAM 8', out)
        self.assertIn('PT read /sd/OS/AUTORUN.TXT -1', out, 'a hook cannot read the OS folder through the core')
        self.assertIn('APP played', out)
        rc, out = self.pt({'PP': {'APP.JSON': manifest(['fs.read', 'fs.read.shared']), 'MAIN.JS': code}}, files=files)
        self.assertTrue(any("capability 'display' is not granted" in l for l in out), out)


class Start(unittest.TestCase):
    def test_newer_api_is_refused_with_a_clear_message(self):
        rc, out = run(RUN, {'NEW': {'APP.JSON': manifest(api=2), 'MAIN.JS': "console.log('T ran')"}}, ['--ms', '50'])
        self.assertEqual(rc, 1)
        self.assertIn('app NEW refused: needs runtime API 2, this is 1', out)
        self.assertEqual(tlines(out), [])
        rc, out = run(RUN, {'NEW': {'APP.JSON': manifest(api=1), 'MAIN.JS': "console.log('T ran')"}}, ['--ms', '50'])
        self.assertEqual((rc, tlines(out)), (0, ['ran']))

    def test_malformed_manifest_is_refused_and_logged(self):
        for name, (text, word) in list(Manifest.BAD.items())[:12]:
            with self.subTest(name):
                rc, out = run(RUN, {'BAD': {'APP.JSON': text, 'MAIN.JS': "console.log('T ran')"}}, ['--ms', '50'])
                self.assertEqual(rc, 1, out)
                self.assertTrue(any(l.startswith('app BAD refused: ') for l in out), out)
                self.assertEqual(tlines(out), [], 'the app never ran')

    def test_missing_entry_is_refused(self):
        rc, out = run(RUN, {'NOE': {'APP.JSON': manifest(entry='GONE.JS'), 'MAIN.JS': 'void 0'}}, ['--ms', '50'])
        self.assertEqual(rc, 1)

    def test_launcher_falls_back_to_the_picker_with_the_reason_on_screen(self):
        apps = {'BAD': {'APP.JSON': manifest(api=9), 'MAIN.JS': 'void 0'}, 'FINE': {'MAIN.JS': 'void 0'}}
        rc, out = run(RUN, apps, ['--launcher', '--ms', '300'], app='/sd/OS/APPS/BAD')
        self.assertEqual(rc, 0, out)
        self.assertIn('app BAD refused: needs runtime API 9, this is 1', out)
        self.assertIn('start error APPS/BAD needs runtime API 9, this is 1', out)
        self.assertIn('app picker BAD,FINE', out, 'the picker lists apps by MAIN.JS or APP.JSON')


AUTO = {'good': "T ran", 'bad': "x"}


class Autorun(unittest.TestCase):
    def az(self, text):
        f = pathlib.Path(tempfile.mkstemp()[1]); f.write_bytes(text.encode())
        try:
            return subprocess.run([CHK, 'autorun', str(f)], capture_output=True, text=True).stdout.strip()
        finally:
            f.unlink()

    def test_line_two_names_the_app(self):
        self.assertEqual(self.az('JS.ELF\nAPPS/MYOS\n'), 'ok /sd/OS/APPS/MYOS')
        self.assertEqual(self.az('JS.ELF\r\nApps/myos \r\n'), 'ok /sd/OS/APPS/MYOS')
        self.assertEqual(self.az('JS.ELF\nPICKER'), 'ok picker')
        self.assertEqual(self.az('JS.ELF\n'), 'ok /sd/OS/APPS/HELLO', 'no name: the default')
        self.assertEqual(self.az('JS.ELF'), 'ok /sd/OS/APPS/HELLO')
        self.assertEqual(self.az(''), 'ok /sd/OS/APPS/HELLO')
        for bad in ('../GAMES', 'APPS/..', 'APPS/A/B', '/dev/log', 'APPS/TOOLONGNAME', 'APPS/', 'APPS/A.B', 'GAMES/X', 'APPS/a~1', '..\\X'):
            with self.subTest(bad):
                self.assertTrue(self.az('JS.ELF\n' + bad + '\n').startswith('err '), bad)

    def start(self, text, apps):
        return run(RUN, apps, ['--autorun', '--ms', '300'], autorun=text)

    def test_named_app_starts(self):
        rc, out = self.start('JS.ELF\nAPPS/GOOD\n', {'GOOD': {'APP.JSON': manifest(), 'MAIN.JS': "console.log('T good ran')"}})
        self.assertEqual(rc, 0, out)
        self.assertIn('autorun OS/APPS/GOOD', out)
        self.assertEqual(tlines(out), ['good ran'])

    def test_every_failed_start_lands_in_the_picker_with_a_reason(self):
        good = {'GOOD': {'MAIN.JS': 'void 0'}}
        cases = {
            'JS.ELF\nAPPS/NOPE\n': ('APPS/NOPE', 'MAIN.JS is not in the app folder'),
            'JS.ELF\n../GAMES\n': ('AUTORUN.TXT', 'use APPS/NAME or PICKER'),
            'JS.ELF\n/dev/log\n': ('AUTORUN.TXT', 'use APPS/NAME or PICKER'),
            'JS.ELF\nAPPS/BADM\n': ('APPS/BADM', 'name'),
            'JS.ELF\nAPPS/NEWER\n': ('APPS/NEWER', 'needs runtime API 4'),
            'JS.ELF\nAPPS/THROWS\n': ('APPS/THROWS', 'threw an error'),
            'JS.ELF\n': ('APPS/HELLO', 'MAIN.JS is not in the app folder'),  # no HELLO on this card
        }
        apps = dict(good, BADM={'APP.JSON': '{"api":1}', 'MAIN.JS': 'void 0'}, NEWER={'APP.JSON': manifest(api=4), 'MAIN.JS': 'void 0'}, THROWS={'MAIN.JS': 'throw new Error("boom")'})
        for text, (who, why) in cases.items():
            with self.subTest(text):
                rc, out = self.start(text, apps)
                self.assertEqual(rc, 0, out)
                self.assertTrue(any(l.startswith('start error ' + who) and why in l for l in out), out)
                self.assertTrue(any(l.startswith('app picker') for l in out), out)

    def test_picker_autorun_and_missing_file(self):
        rc, out = self.start('JS.ELF\nPICKER\n', {'GOOD': {'MAIN.JS': 'void 0'}})
        self.assertIn('autorun PICKER', out); self.assertFalse(any(l.startswith('start error') for l in out))
        d = card({'GOOD': {'MAIN.JS': 'void 0'}})
        try:
            p = subprocess.run([str(RUN), str(d), 'x', '--autorun', '--ms', '200', '--vclock'], capture_output=True, text=True)  # no AUTORUN.TXT: HELLO is absent
        finally:
            shutil.rmtree(d, ignore_errors=True)
        self.assertIn('start error APPS/HELLO', p.stdout)


class Checker(unittest.TestCase):
    def test_app_check_on_a_card(self):
        d = card({'OK1': {'APP.JSON': manifest(['input']), 'MAIN.JS': ''}, 'BAD1': {'APP.JSON': manifest(['wifi']), 'MAIN.JS': ''}, 'NOENT': {'APP.JSON': manifest(entry='Z.JS'), 'MAIN.JS': ''}},
                 autorun='JS.ELF\nAPPS/OK1\n')
        try:
            p = subprocess.run([sys.executable, str(JS / 'tools/app_check.py'), '--card', str(d)], capture_output=True, text=True)
            self.assertEqual(p.returncode, 1)
            self.assertIn('OK1: ok name=Test', p.stdout)
            self.assertIn('BAD1: err unknown capability "wifi"', p.stdout)
            self.assertIn('NOENT: err Z.JS is not in the app folder', p.stdout)
            self.assertIn('AUTORUN: APPS/OK1 ok', p.stdout)
            p = subprocess.run([sys.executable, str(JS / 'tools/app_check.py'), str(d / 'OS/APPS/OK1')], capture_output=True, text=True)
            self.assertEqual(p.returncode, 0, p.stdout)
        finally:
            shutil.rmtree(d, ignore_errors=True)


if __name__ == '__main__':
    RUN, PT, CHK = sys.argv[1:4]
    del sys.argv[1:4]
    unittest.main()
