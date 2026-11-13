#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 5 fuzz pass over the validators with no JS in the way: APP.JSON (every truncation, every single-byte edit, type-confused
fields, random bytes, deep nesting), the AUTORUN.TXT line and the path policy. The C validators run in one process (appcheck batch);
the manifest is also compared with tools/app_check.py (accept or refuse must agree, and an accept must say the same thing), and
the path policy is held to security properties written down here, independent of the C code: whatever a random path is, an
allowed one never leaves the app's own folder, its save folder or its cache file (or, with fs.read.shared, the games folders), and
never holds a '..', a '.', an empty component or a character the kernel would treat specially.
Usage: fuzz_run.py <appcheck>"""
import json, pathlib, random, re, subprocess, sys, unittest

JS = pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0, str(JS / 'tools'))
import app_check
CHK = None

FULL = json.dumps({'name': 'Fuzz App', 'version': '1.2.3', 'entry': 'MAIN.JS', 'api': 1, 'description': 'a test',
                   'capabilities': ['fs.read', 'fs.write.saves', 'display', 'input', 'audio', 'eink', 'apps.launch'], 'x-note': 'hi'}, indent=1)
MIN = '{"name":"M","api":1}'
BASES = [FULL, MIN, '{"schema":1,"name":"Two","api":1,"capabilities":[],"entry":"A-B_1.JS","version":"0"}']


def batch(lines):
    """Feeds lines to appcheck batch; returns its output lines (one each). A crash fails the test with the line that was running."""
    p = subprocess.run([str(CHK), 'batch'], input=('\n'.join(lines) + '\n').encode(), capture_output=True, timeout=600)
    out = p.stdout.decode('latin-1').split('\n')[:-1]   # not splitlines(): that also splits on VT, FF and 0x1c to 0x1e
    if p.returncode != 0 or len(out) != len(lines):
        k = len(out)
        raise AssertionError('appcheck batch died (rc %s) on case %d: %r' % (p.returncode, k, lines[k] if k < len(lines) else None))
    return out


def hx(b):
    return b.hex() if isinstance(b, (bytes, bytearray)) else b.encode('latin-1').hex()


def manifests():
    """Every case once, as bytes."""
    cases = []
    rnd = random.Random(11)
    inter = b'"{}[]:,\\ 0123456789xtfn-.eE\x00\x7f\x80\xff\n\t/u'
    for base in BASES:
        b = base.encode()
        for i in range(len(b) + 1):
            cases.append(b[:i])                                  # every truncation
        for i in range(len(b)):
            cases.append(b[:i] + b[i + 1:])                      # every deletion
            for c in inter:
                cases.append(b[:i] + bytes([c]) + b[i + 1:])     # every substitution
        for i in range(0, len(b), 3):
            for c in b'"{},:[ \\0x':
                cases.append(b[:i] + bytes([c]) + b[i:])         # insertions
    pool = [None, True, False, 0, 1, 2, -1, 65535, 65536, 1.5, 1e300, '', 'x', 'x' * 15, 'x' * 16, 'x' * 32, 'x' * 33, 'x' * 121, [], {}, ['fs.read'], ['fs.read', 'fs.read'],
            ['nope'], ['fs.read'] * 17, [1], [None], '1.0', '1.2.3.4', '99999.0', 'A.JS', 'A.JSX', '.JS', 'ABCDEFGHI.JS', 'a b', 'tab\there', [[[]]], {'a': {'b': {}}}]
    keys = ['name', 'api', 'version', 'entry', 'description', 'capabilities', 'schema', 'x-extra', 'unknown', 'Name', '', 'x' * 30]
    for _ in range(3000):
        d = {}
        for k in rnd.sample(keys, rnd.randint(0, 6)):
            d[k] = rnd.choice(pool)
        d.setdefault('name', rnd.choice(['ok', 'x' * 33, None, 7]))
        if rnd.random() < .8:
            d.setdefault('api', rnd.choice([1, 1, 1, 2, 0, '1']))
        try:
            cases.append(json.dumps(d, indent=rnd.choice([None, 1])).encode())
        except ValueError:
            pass
    for _ in range(1500):
        cases.append(bytes(rnd.randrange(256) for _ in range(rnd.choice([0, 1, 2, 5, 40, 400, 2047, 2048, 2049, 3000, 4000]))))
    for n in list(range(1, 80)) + [150, 400, 1500]:
        cases.append(b'[' * n + b']' * n)
        cases.append(b'{"name":"a","api":1,"x-a":' + b'{"x":' * n + b'0' + b'}' * n + b'}')
    cases.append(b'{"name":"a","api":1}' + b' ' * 2100)
    cases.append(b'{"name":"a","api":1,"description":"' + b'x' * 2100 + b'"}')
    return cases


class Manifest(unittest.TestCase):
    def test_every_edit_agrees_with_the_python_checker_and_nothing_crashes(self):
        cases = manifests()
        out = batch(['M ' + hx(c) for c in cases])
        accepted = 0
        bad = []
        for c, o in zip(cases, out):
            self.assertTrue(o.startswith('ok ') or o.startswith('err '), (c[:60], o))
            if len(c) > 4000:
                continue
            py = app_check.line(app_check.check_manifest(c, api=65535))
            ok = o.startswith('ok ')
            accepted += ok
            if ok != py.startswith('ok ') or (ok and o != py):
                bad.append((c[:200], o, py))
        print('\nFUZZ manifests %d cases, %d accepted, %d disagreements' % (len(cases), accepted, len(bad)))
        self.assertGreater(accepted, 200, 'the fuzz never produced a valid manifest')
        self.assertGreater(len(cases) - accepted, 5000)
        self.assertEqual(bad[:5], [], '%d cases where the C and Python checkers differ' % len(bad))

    def test_accepted_manifests_stay_inside_their_limits(self):
        cases = manifests()
        out = batch(['M ' + hx(c) for c in cases])
        for c, o in zip(cases, out):
            if not o.startswith('ok '):
                continue
            m = re.fullmatch(r'ok name=(.*) version=(.*) entry=(.*) api=(\d+) caps=(.*)', o)
            self.assertTrue(m, o)
            name, version, entry, api, caps = m.groups()
            self.assertTrue(1 <= len(name) <= 32 and all(0x20 <= ord(ch) < 0x7f for ch in name), o)
            self.assertRegex(version, r'^\d{1,4}(\.\d{1,4}){0,2}$')
            self.assertRegex(entry, r'^[A-Z0-9_-]{1,8}\.JS$')
            self.assertTrue(1 <= int(api) <= 65535)
            self.assertTrue(len(c) <= 2048, 'a manifest past 2048 bytes was accepted')
            for cap in filter(None, caps.split(',')):
                self.assertIn(cap, app_check.CAPS)


class Autorun(unittest.TestCase):
    def test_random_lines_never_name_anything_but_an_app_folder_or_the_picker(self):
        rnd = random.Random(5)
        pool = ['JS.ELF', 'APPS/HELLO', 'PICKER', 'picker', 'APPS/', 'APPS/..', 'APPS/A B', 'APPS/ABCDEFGHI', 'APPS/a-b_1', '../OS', 'APPS//X', 'APPS/X/Y', '', ' ', 'APPS\\X', 'APPS/X.JS', 'apps/x', '\x00', 'APPS/%s' % ('A' * 40)]
        cases = []
        for _ in range(4000):
            t = rnd.choice(['\n', '\r\n', '\n\n', '']).join(rnd.choice(pool) for _ in range(rnd.randint(0, 4)))
            if rnd.random() < .3:
                t = ''.join(rnd.choice([c, chr(rnd.randrange(128))]) for c in t)
            cases.append(t.encode('latin-1', 'replace'))
        cases += [bytes(rnd.randrange(256) for _ in range(rnd.randint(0, 600))) for _ in range(1500)]
        out = batch(['A ' + hx(c) if c else 'A' for c in cases])
        picked = 0
        for c, o in zip(cases, out):
            if o.startswith('ok '):
                self.assertRegex(o, r'^ok (picker|/sd/OS/APPS/[A-Za-z0-9_-]{1,8})$', (c, o))
                picked += 1
            else:
                self.assertTrue(o.startswith('err '), (c, o))
        self.assertGreater(picked, 100)


SEG = ['OS', 'os', 'APPS', 'apps', 'PATHS', 'paths', 'PATHSX', 'PATH', 'OTHER', 'SAVES', 'CACHE', 'GAMES', 'G1', 'MAIN.JS', 'S1.SAV', 'A.TXT', 'LONGNAME9.JS', '..', '.', '', '...', ' ', 'A B', 'A.', 'A ', '~1', 'A~1.TXT',
       'C:', 'a*b', 'a?b', 'a|b', 'a"b', 'a<b', '%2e%2e', '%2F', '\\', '..\\x', 'x\ny', 'x\x7fy', 'é', 'ADMIN', 'sd', 'dev', 'fb0', 'proc']
DEFAULT_ID = 'PATHS'
BAD_CHARS = re.compile(r'[\\:*?"<>|~]|[^\x20-\x7e]')   # what the kernel or the card would treat specially, or no name has


class Paths(unittest.TestCase):
    def cases(self, n=12000):
        rnd = random.Random(3)
        out = []
        for _ in range(n):
            head = rnd.choice(['/sd', '/sd', '/sd', '/SD', '/sd/', '', 'sd', '/dev', '//sd', '/sd/OS/APPS/PATHS', '/sd/OS/SAVES/PATHS', '/sd/OS/CACHE', '/sd/OS/APPS', '/sd/GAMES'])
            segs = [rnd.choice(SEG) for _ in range(rnd.randint(0, 5))]
            if rnd.random() < .5:
                segs = ['OS', rnd.choice(['APPS', 'SAVES', 'CACHE']), rnd.choice(['PATHS', 'OTHER', 'PATHSX'])] + segs[:2]
            p = head + ('/' if segs else '') + '/'.join(segs)
            if rnd.random() < .15:
                p += rnd.choice(['/', '.', ' ', '/..', '/.', '/' + 'a' * 100])
            out.append(p)
        return out

    def run_policy(self, caps, mode, paths, pid=DEFAULT_ID):
        out = batch(['P %s %s %s %s' % (pid, caps, mode, hx(p) if p else '') for p in paths][:])
        return [o == 'allow' for o in out], out

    def clean(self, p):
        self.assertTrue(p.isascii() and all(0x20 < ord(c) < 0x7f or c == ' ' for c in p), repr(p))
        parts = p.split('/')
        self.assertEqual(parts[0], '', p)
        for s in parts[1:]:
            self.assertNotIn(s, ('', '.', '..'), p)
            self.assertFalse(s.endswith('.') or s.endswith(' '), p)
            self.assertFalse(BAD_CHARS.search(s), p)

    def test_a_read_never_leaves_the_apps_own_folders(self):
        paths = self.cases()
        allowed, out = self.run_policy('fs.read', 'r', paths)
        n = 0
        for p, ok in zip(paths, allowed):
            if not ok:
                continue
            n += 1
            self.clean(p)
            low = p.lower()
            self.assertTrue(re.match(r'^/sd/os/(apps|saves)/paths(/|$)', low), p)
        self.assertGreater(n, 200)
        print('\nFUZZ paths %d cases, %d reads allowed' % (len(paths), n))

    def test_a_shared_read_never_reaches_the_os_folder_or_another_app(self):
        paths = self.cases() + ['/sd/GAMES/G1/GAME.GAM', '/sd/games/g1/x', '/sd/OS/APPS/OTHER/MAIN.JS', '/sd/os/AUTORUN.TXT']
        allowed, _ = self.run_policy('fs.read,fs.read.shared', 'r', paths)
        for p, ok in zip(paths, allowed):
            if not ok:
                continue
            self.clean(p)
            low = p.lower()
            self.assertTrue(re.match(r'^/sd/os/(apps|saves)/paths(/|$)', low) or (re.match(r'^/sd/[^/]+(/|$)', low) and not re.match(r'^/sd/os(/|$)', low)), p)

    def test_a_write_is_one_save_file_or_the_cache_file_and_nothing_else(self):
        paths = self.cases(20000) + ['/sd/OS/SAVES/PATHS/S1.SAV', '/sd/OS/CACHE/PATHS', '/sd/os/saves/paths/a.b', '/sd/OS/SAVES/PATHS/12345678.123']
        allowed, _ = self.run_policy('fs.write.saves,fs.write.cache,fs.read', 'w', paths)
        n = 0
        for p, ok in zip(paths, allowed):
            if not ok:
                continue
            n += 1
            self.clean(p)
            self.assertRegex(p.lower(), r'^/sd/os/(saves/paths/.+|cache/paths)$', p)   # the kernel then insists on a canonical 8.3 name, one level deep
        self.assertGreaterEqual(n, 3)
        # without the grants nothing is writable, however the path is spelled
        for caps in ('fs.read', 'none', 'fs.read,fs.read.shared', 'fs.write.cache', 'fs.write.saves'):
            allowed, _ = self.run_policy(caps, 'w', paths)
            for p, ok in zip(paths, allowed):
                if ok:
                    self.assertTrue(('saves' in caps and '/saves/' in p.lower()) or ('cache' in caps and '/cache/' in p.lower()), (caps, p))

    def test_no_path_reaches_another_apps_folders(self):
        # the same random paths for apps with confusable ids: whatever one may read, the others may not unless it is in their own folder
        paths = self.cases(8000)
        for pid in ('A', 'PATH', 'PATHSX', 'path-s', 'P_1'):
            allowed, _ = self.run_policy('fs.read,fs.write.saves,fs.write.cache', 'r', paths, pid)
            for p, ok in zip(paths, allowed):
                if ok:
                    self.assertTrue(re.match(r'^/sd/os/((apps|saves)/%s(/|$)|cache/%s$)' % (re.escape(pid.lower()), re.escape(pid.lower())), p.lower()), (pid, p))

    def test_random_bytes_never_crash_the_policy(self):
        rnd = random.Random(9)
        paths = [''.join(chr(rnd.randrange(1, 256)) for _ in range(rnd.choice([1, 3, 20, 90, 95, 96, 97, 200, 1000, 4000]))) for _ in range(3000)]
        paths += ['/sd/' + 'a' * n for n in (80, 90, 94, 95, 96, 100, 500, 4000)] + ['/sd/' + '/'.join(['a'] * n) for n in (10, 40, 60, 200)]
        allowed, out = self.run_policy('all', 'r', paths)
        self.assertEqual(len(out), len(paths))
        for p, ok in zip(paths, allowed):
            if ok:
                self.assertLessEqual(len(p), 95, 'a path past the length limit was allowed')
                self.clean(p)


if __name__ == '__main__':
    CHK = pathlib.Path(sys.argv.pop(1)).resolve()
    unittest.main(verbosity=1)
