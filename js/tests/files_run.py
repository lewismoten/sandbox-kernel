#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 4a tests: file handles (binary reads, seek bounds, partial reads, stat, readdir meta), the staged writer (commit,
abort, stop, quota, names, the kernel's one writer and caps) and capability gating, all on the host with the kernel's write
rules modeled in tests/host/jsrun.c. Usage: files_run.py <jsrun>"""
import json, pathlib, re, shutil, subprocess, sys, tempfile, unittest

RUN = None
APP = '/sd/OS/APPS/FT'


def go(code, caps=('fs.read', 'fs.write.saves'), args=(), files=None, dirs=(), ms='300', app='FT'):
    """Runs one app on a temporary card. files: {'sd/path': bytes|str}. Returns (rc, T lines, other log lines, card path)."""
    d = pathlib.Path(tempfile.mkdtemp())
    (d / 'OS/APPS' / app).mkdir(parents=True)
    (d / 'OS/APPS' / app / 'APP.JSON').write_text(json.dumps({'name': 'Files', 'api': 1, 'capabilities': list(caps)}))
    (d / 'OS/APPS' / app / 'MAIN.JS').write_text(code)
    for x in dirs:
        (d / x).mkdir(parents=True, exist_ok=True)
    for f, b in (files or {}).items():
        (d / f).parent.mkdir(parents=True, exist_ok=True)
        (d / f).write_bytes(b if isinstance(b, bytes) else b.encode())
    p = subprocess.run([str(RUN), str(d), '/sd/OS/APPS/' + app, '--ms', ms, '--vclock'] + list(args), capture_output=True, text=True, timeout=120)
    out = [l[3:] if l.startswith('JS ') else l for l in (p.stdout + p.stderr).splitlines()]
    return p.returncode, [l[2:] for l in out if l.startswith('T ')], [l for l in out if not l.startswith('T ')], d


def card_files(d):
    return sorted(str(p.relative_to(d)) for p in d.rglob('*') if p.is_file() and 'APPS' not in p.parts)


class Keys(dict):  # the apps' T() lines use underscores for spaces in a key
    def __getitem__(self, k):
        return dict.__getitem__(self, k.replace(' ', '_'))


def run_ok(self, code, **kw):
    rc, t, rest, d = go(code, **kw)
    self.addCleanup(shutil.rmtree, d, True)
    self.assertEqual(rc, 0, rest)
    return Keys({l.split(' ', 1)[0]: l.split(' ', 1)[1] if ' ' in l else '' for l in t}), rest, d


PRE = """
const T = (k, v) => console.log('T ' + k.replace(/ /g, '_') + ' ' + v);
const E = (k, fn) => { try { const r = fn(); T(k, 'ok ' + (r === undefined ? '' : r)); } catch (e) { T(k, (e.code || e.name) + ' | ' + e.message); } };
"""
BIN = bytes(range(256)) * 3  # 768 bytes


class Reads(unittest.TestCase):
    def test_binary_round_trip_and_all_read_shapes(self):
        code = PRE + """
const f = fs.open('/sd/OS/APPS/FT/BIN.DAT');
T('size', f.size + ' pos ' + f.position);
const a = f.read(300);
T('read', (a instanceof Uint8Array) + ' ' + a.length + ' ' + a[0] + ' ' + a[255] + ' ' + a[256] + ' pos ' + f.position);
const into = new Uint8Array(1000);
const n = f.readInto(into, 10, 200);
T('into', n + ' ' + into[9] + ' ' + into[10] + ' ' + into[11] + ' ' + into[209] + ' ' + into[210] + ' pos ' + f.position);
const rest = f.read();
T('rest', rest.length + ' ' + f.position + ' last ' + rest[rest.length - 1]);
T('eof', f.read(10).length + ' ' + f.readInto(into));
f.close();
const all = fs.readBytes('/sd/OS/APPS/FT/BIN.DAT');
T('bytes', all.length + ' ' + all.every((v, i) => v === i % 256));
const i32 = new Int32Array(4);
const g = fs.open('/sd/OS/APPS/FT/BIN.DAT'); T('i32', g.readInto(i32) + ' ' + i32[0]); g.close();
const buf = new ArrayBuffer(8); const h = fs.open('/sd/OS/APPS/FT/BIN.DAT'); T('ab', h.readInto(buf, 0, 8) + ' ' + new Uint8Array(buf)[7]); h.close();
sys.exit(0);
"""
        t, _, _ = run_ok(self, code, files={'OS/APPS/FT/BIN.DAT': BIN})
        self.assertEqual(t['size'], '768 pos 0')
        self.assertEqual(t['read'], 'true 300 0 255 0 pos 300')
        self.assertEqual(t['into'], '200 0 44 45 243 0 pos 500')  # bytes 300 to 499 land at into[10] to into[209]; into[9] is untouched
        self.assertEqual(t['rest'], '268 768 last 255')
        self.assertEqual(t['eof'], '0 0')
        self.assertEqual(t['bytes'], '768 true')
        self.assertEqual(t['i32'], '16 50462976')  # 0x03020100, little endian
        self.assertEqual(t['ab'], '8 7')

    def test_text_reads_and_partial_reads(self):
        code = PRE + """
const f = fs.open('/sd/OS/APPS/FT/T.TXT');
T('a', f.text(5)); T('b', f.text(3)); T('pos', f.position);
T('c', JSON.stringify(f.text())); T('d', JSON.stringify(f.text()));
T('over', f.read(1000).length);
f.seek(0); T('again', f.read(1000).length);
f.seek(2); T('utf8', f.size);
E('neg', () => f.read(-1)); E('frac', () => f.read(1.5));
f.close();
const g = fs.open('/sd/OS/APPS/FT/U.TXT'); T('u', g.text()); g.close();
sys.exit(0);
"""
        t, _, _ = run_ok(self, code, files={'OS/APPS/FT/T.TXT': 'HELLO WORLD!\n', 'OS/APPS/FT/U.TXT': 'café ✓'})
        self.assertEqual((t['a'], t['b'], t['pos']), ('HELLO', ' WO', '8'))
        self.assertEqual(t['c'], '"RLD!\\n"')
        self.assertEqual(t['d'], '""')
        self.assertEqual((t['over'], t['again']), ('0', '13'))
        self.assertIn('RangeError', t['neg']); self.assertIn('RangeError', t['frac'])
        self.assertEqual(t['u'], 'café ✓')

    def test_seek_bounds(self):
        code = PRE + """
const f = fs.open('/sd/OS/APPS/FT/BIN.DAT');
E('mid', () => f.seek(100)); T('after', f.read(2)[0] + ' ' + f.position);
E('end', () => f.seek(768)); T('at end', f.read(5).length);
E('past', () => f.seek(769)); E('neg', () => f.seek(-1)); E('frac', () => f.seek(1.5)); E('nan', () => f.seek(NaN)); E('none', () => f.seek());
T('unchanged', f.position);
E('start', () => f.seek(0)); T('again', f.read(1)[0]);
f.close();
E('closed', () => f.seek(0)); E('closed read', () => f.read(1)); E('closed size', () => f.size); E('closed close', () => f.close());
sys.exit(0);
"""
        t, _, _ = run_ok(self, code, files={'OS/APPS/FT/BIN.DAT': BIN})
        self.assertEqual(t['mid'], 'ok 100'); self.assertEqual(t['after'], '100 102')
        self.assertEqual(t['end'], 'ok 768'); self.assertEqual(t['at end'], '0')
        for k in ('past', 'neg', 'frac', 'nan'):
            self.assertTrue(t[k].startswith('RangeError'), (k, t[k]))
        self.assertTrue(t['none'].startswith('TypeError'))
        self.assertEqual(t['unchanged'], '768')
        self.assertEqual(t['start'], 'ok 0'); self.assertEqual(t['again'], '0')
        for k in ('closed', 'closed read', 'closed size', 'closed close'):
            self.assertIn('EBADF', t[k], k)

    def test_stat_and_readdir_meta(self):
        code = PRE + """
T('stat', JSON.stringify(fs.stat('/sd/OS/APPS/FT/BIN.DAT')));
T('stat dir', JSON.stringify(fs.stat('/sd/OS/APPS/FT/sub')).replace(/"modified[^}]*/, ''));
T('stat case', fs.stat('/sd/os/apps/ft/bin.dat').size);
E('missing', () => fs.stat('/sd/OS/APPS/FT/NOPE.BIN'));
E('missing parent', () => fs.stat('/sd/OS/APPS/FT/NOPE/X.BIN'));
E('open dir', () => fs.open('/sd/OS/APPS/FT/sub'));
E('open missing', () => fs.open('/sd/OS/APPS/FT/NOPE.BIN'));
const m = fs.readdir('/sd/OS/APPS/FT', { meta: true }).map(e => e.name + ':' + e.type + ':' + e.size).sort();
T('meta', m.join(','));
T('names', fs.readdir('/sd/OS/APPS/FT').sort().join(','));
T('long', fs.readdir('/sd/OS/APPS/FT/sub', { meta: true }).map(e => e.name + ':' + e.size).join(','));
E('readdir missing', () => fs.readdir('/sd/OS/APPS/FT/NOPE', { meta: true }));
E('outside', () => fs.stat('/sd/OS/APPS/OTHER/X'));
sys.exit(0);
"""
        t, _, _ = run_ok(self, code, files={'OS/APPS/FT/BIN.DAT': BIN, 'OS/APPS/FT/sub/A long name.txt': 'abc'})
        st = json.loads(t['stat'])
        self.assertEqual((st['name'], st['type'], st['size']), ('BIN.DAT', 'file', 768))
        self.assertIn('"type":"dir"', t['stat dir']); self.assertEqual(t['stat case'], '768')
        self.assertTrue(t['missing'].startswith('ENOENT')); self.assertTrue(t['missing parent'].startswith('ENOENT'))
        self.assertTrue(t['open dir'].startswith('EINVAL')); self.assertIn('no file', t['open missing'])
        for want in ('APP.JSON:file:', 'MAIN.JS:file:', 'BIN.DAT:file:768', 'sub:dir:0'):
            self.assertIn(want, t['meta'])
        self.assertEqual(sorted(t['names'].upper().split(',')), ['APP.JSON', 'BIN.DAT', 'MAIN.JS', 'SUB'])
        self.assertIn('A long name.txt:3', t['long'])
        self.assertTrue(t['readdir missing'].startswith('ENOENT')); self.assertIn('EACCES', t['outside'])


class Writer(unittest.TestCase):
    SAV = '/sd/OS/SAVES/FT/'

    def test_commit_publishes_and_the_data_round_trips(self):
        code = PRE + """
const SAV = '/sd/OS/SAVES/FT/';
T('write', fs.write(SAV + 'S1.SAV', 'hello'));
const w = fs.createWriter(SAV + 'bin.dat');
T('w1', w.write(new Uint8Array([1, 2, 3, 255])));
T('w2', w.write(new Uint8Array([9]).buffer));
T('w3', w.write('\\u00e9'));
T('size', w.size + ' ' + w.path);
T('commit', w.commit());
E('after', () => w.write('x')); E('commit twice', () => w.commit());
T('abort twice', w.abort() === undefined);
const f = fs.open(SAV + 'BIN.DAT'); const b = f.read(); f.close();
T('back', Array.from(b).join(','));
T('text', fs.readFile(SAV + 'S1.SAV'));
T('replace', fs.write(SAV + 'S1.SAV', 'longer text!') + ' ' + fs.readFile(SAV + 'S1.SAV'));
T('empty', fs.write(SAV + 'EMPTY', new Uint8Array(0)) + ' ' + fs.stat(SAV + 'EMPTY').size);
T('list', fs.readdir('/sd/OS/SAVES/FT', { meta: true }).map(e => e.name + ':' + e.size).sort().join(','));
sys.exit(0);
"""
        t, rest, d = run_ok(self, code)
        self.assertEqual(t['write'], '5'); self.assertEqual(t['w1'], '4'); self.assertEqual(t['w2'], '5'); self.assertEqual(t['w3'], '7')
        self.assertEqual(t['size'], '7 /sd/OS/SAVES/FT/BIN.DAT'); self.assertEqual(t['commit'], '7')
        self.assertIn('EBADF', t['after']); self.assertIn('EBADF', t['commit twice']); self.assertEqual(t['abort twice'], 'true')
        self.assertEqual(t['back'], '1,2,3,255,9,195,169')
        self.assertEqual(t['text'], 'hello'); self.assertEqual(t['replace'], '12 longer text!'); self.assertEqual(t['empty'], '0 0')
        self.assertEqual(t['list'], 'BIN.DAT:7,EMPTY:0,S1.SAV:12')
        self.assertEqual((d / 'OS/SAVES/FT/BIN.DAT').read_bytes(), bytes([1, 2, 3, 255, 9, 0xc3, 0xa9]))
        self.assertFalse([l for l in rest if 'left' in l], rest)

    def test_cache_file_with_its_own_capability(self):
        code = PRE + """
T('cache', fs.write('/sd/OS/CACHE/FT', 'tmp'));
E('saves', () => fs.write('/sd/OS/SAVES/FT/A.SAV', 'x'));
E('other', () => fs.write('/sd/OS/CACHE/OTHER', 'x'));
sys.exit(0);
"""
        t, _, d = run_ok(self, code, caps=('fs.write.cache',))
        self.assertEqual(t['cache'], '3'); self.assertIn('EACCES', t['saves']); self.assertIn('no fs.write.saves capability', t['saves'])
        self.assertTrue(t['other'].startswith('EROFS'), t['other'])
        self.assertEqual((d / 'OS/CACHE/FT').read_text(), 'tmp')

    def test_abort_drop_and_stop_leave_the_card_untouched(self):
        code = PRE + """
const SAV = '/sd/OS/SAVES/FT/';
const a = fs.createWriter(SAV + 'A.SAV'); a.write('never'); T('size', a.size); a.abort();
E('after abort', () => a.write('x'));
let c = fs.createWriter(SAV + 'C.SAV'); c.write('at stop');
globalThis.keep = c;
E('busy', () => fs.createWriter(SAV + 'D.SAV'));
sys.exit(0);
"""
        t, rest, d = run_ok(self, code)
        self.assertEqual(t['size'], '5'); self.assertIn('EBADF', t['after abort']); self.assertTrue(t['busy'].startswith('EBUSY'))
        self.assertEqual(card_files(d), [], 'no save, no folder: nothing reached the card')
        self.assertFalse((d / 'OS/SAVES').exists())
        self.assertTrue(any('left 1 handle(s) open: file 1' in l for l in rest), rest)

    def test_quota_per_file_and_per_folder(self):
        code = PRE + """
const SAV = '/sd/OS/SAVES/FT/';
T('quota', fs.writeQuota());
const w = fs.createWriter(SAV + 'A.SAV');
T('w600', w.write(new Uint8Array(600)));
E('over', () => w.write(new Uint8Array(401))); T('size kept', w.size);
T('w400', w.write(new Uint8Array(400)));
T('commit', w.commit());
E('second file', () => fs.write(SAV + 'B.SAV', new Uint8Array(1)));
E('replace smaller', () => fs.write(SAV + 'A.SAV', new Uint8Array(10)));
E('then B', () => fs.write(SAV + 'B.SAV', new Uint8Array(900)));
E('write over', () => fs.write(SAV + 'C.SAV', new Uint8Array(2000)));
sys.exit(0);
"""
        t, _, d = run_ok(self, code, args=['--write-quota', '1000'])
        self.assertEqual(t['quota'], '1000'); self.assertEqual(t['w600'], '600'); self.assertTrue(t['over'].startswith('EDQUOT'), t['over'])
        self.assertIn('1000 bytes', t['over']); self.assertEqual(t['size kept'], '600'); self.assertEqual(t['commit'], '1000')
        self.assertTrue(t['second file'].startswith('EDQUOT'), 'the folder is full'); self.assertEqual(t['replace smaller'], 'ok 10')
        self.assertEqual(t['then B'], 'ok 900'); self.assertTrue(t['write over'].startswith('EDQUOT'))
        self.assertEqual(card_files(d), ['OS/SAVES/FT/A.SAV', 'OS/SAVES/FT/B.SAV'])

    def test_default_quota_is_256_kib_and_one_mib_is_the_hard_cap(self):
        code = PRE + """
const SAV = '/sd/OS/SAVES/FT/';
T('quota', fs.writeQuota());
E('256k', () => fs.write(SAV + 'A.SAV', new Uint8Array(262144)));
E('256k+1', () => fs.write(SAV + 'B.SAV', new Uint8Array(262145)));
sys.exit(0);
"""
        t, _, _ = run_ok(self, code)
        self.assertEqual(t['quota'], '262144'); self.assertEqual(t['256k'], 'ok 262144'); self.assertTrue(t['256k+1'].startswith('EDQUOT'))
        code = PRE + "E('big', () => fs.write('/sd/OS/SAVES/FT/A.SAV', new Uint8Array(1048577))); E('1m', () => fs.write('/sd/OS/SAVES/FT/B.SAV', new Uint8Array(1048576))); sys.exit(0);"
        t, _, _ = run_ok(self, code, args=['--write-quota', '4000000', '--heap', str(8 << 20)])
        self.assertTrue(t['big'].startswith('ENOSPC'), t['big']); self.assertIn('1 MiB', t['big']); self.assertEqual(t['1m'], 'ok 1048576')

    def test_names_and_scope(self):
        names = {
            'lower': ('/sd/OS/saves/ft/s1.sav', 'ok'), 'noext': ('/sd/OS/SAVES/FT/SLOT-1', 'ok'), 'eight': ('/sd/OS/SAVES/FT/ABCDEFGH.123', 'ok'),
            'long stem': ('/sd/OS/SAVES/FT/ABCDEFGHI.SAV', 'ENAMETOOLONG'), 'long ext': ('/sd/OS/SAVES/FT/A.SAVE', 'ENAMETOOLONG'),
            'two dots': ('/sd/OS/SAVES/FT/A.B.C', 'EINVAL'), 'space': ('/sd/OS/SAVES/FT/A B.SAV', 'EINVAL'), 'plus': ('/sd/OS/SAVES/FT/A+B.SAV', 'EINVAL'),
            'dot first': ('/sd/OS/SAVES/FT/.SAV', 'EINVAL'), 'deeper': ('/sd/OS/SAVES/FT/SUB/A.SAV', 'EINVAL'),
            'folder': ('/sd/OS/SAVES/FT', 'EROFS'), 'trailing dot': ('/sd/OS/SAVES/FT/A.', 'EROFS'),
            'cache long': ('/sd/OS/CACHE/FT.TOOLONG', 'EROFS'),
            'own folder': ('/sd/OS/APPS/FT/NEW.TXT', 'EROFS'), 'games': ('/sd/GAMES/G1/X.SAV', 'EROFS'), 'root': ('/sd/X.TXT', 'EROFS'),
            'other app': ('/sd/OS/SAVES/OTHER/X.SAV', 'EROFS'), 'dotdot': ('/sd/OS/SAVES/FT/../OTHER/X.SAV', 'EROFS'), 'device': ('/dev/log', 'EROFS'),
            'autorun': ('/sd/OS/AUTORUN.TXT', 'EROFS'),
        }
        code = PRE + "const N = " + json.dumps({k: v[0] for k, v in names.items()}) + ";\nfor (const k in N) E(k, () => fs.write(N[k], 'x'));\nsys.exit(0);"
        t, _, d = run_ok(self, code, caps=('fs.write.saves',))
        for k, (_, want) in names.items():
            self.assertTrue(t[k].startswith(want), (k, want, t[k]))
        self.assertEqual(card_files(d), ['OS/SAVES/FT/ABCDEFGH.123', 'OS/SAVES/FT/S1.SAV', 'OS/SAVES/FT/SLOT-1'], 'lowercase is upcased to the kernel name')
        self.assertIn('read-only', t['own folder'])

    def test_a_missing_grant_names_the_capability(self):
        code = PRE + "E('write', () => fs.write('/sd/OS/SAVES/FT/A.SAV', 'x')); E('create', () => fs.createWriter('/sd/OS/SAVES/FT/A.SAV')); E('open', () => fs.open('/sd/OS/APPS/FT/MAIN.JS')); E('stat', () => fs.stat('/sd/OS/APPS/FT/MAIN.JS')); E('meta', () => fs.readdir('/sd/OS/APPS/FT', { meta: true })); E('bytes', () => fs.readBytes('/sd/OS/APPS/FT/MAIN.JS')); sys.exit(0);"
        t, _, d = run_ok(self, code, caps=('display',))
        for k in ('write', 'create'):
            self.assertIn("capability 'fs.write.saves' is not granted", t[k], k)
        for k in ('open', 'stat', 'meta', 'bytes'):
            self.assertIn("capability 'fs.read' is not granted", t[k], k)
        self.assertEqual(card_files(d), [])
        code = PRE + "E('r', () => fs.readBytes('/sd/OS/APPS/FT/MAIN.JS').length > 0); E('shared', () => fs.stat('/sd/GAMES/G1/GAME.GAM').size); sys.exit(0);"
        t, _, _ = run_ok(self, code, caps=('fs.read',), files={'GAMES/G1/GAME.GAM': 'abc'})
        self.assertEqual(t['r'], 'ok true'); self.assertIn('fs.read.shared', t['shared'])
        t, _, _ = run_ok(self, code, caps=('fs.read', 'fs.read.shared'), files={'GAMES/G1/GAME.GAM': 'abc'})
        self.assertEqual(t['shared'], 'ok 3')

    def test_one_writer_at_a_time(self):
        code = PRE + """
const SAV = '/sd/OS/SAVES/FT/';
const a = fs.createWriter(SAV + 'A.SAV');
E('second', () => fs.createWriter(SAV + 'B.SAV'));
E('fs.write', () => fs.write(SAV + 'B.SAV', 'x'));
a.write('one'); T('commit', a.commit());
E('after', () => fs.write(SAV + 'B.SAV', 'two'));
const c = fs.createWriter(SAV + 'C.SAV'); c.abort();
E('after abort', () => fs.write(SAV + 'C.SAV', 'three'));
sys.exit(0);
"""
        t, _, d = run_ok(self, code)
        self.assertTrue(t['second'].startswith('EBUSY'), t['second']); self.assertTrue(t['fs.write'].startswith('EBUSY'))
        self.assertEqual(t['commit'], '3'); self.assertEqual(t['after'], 'ok 3'); self.assertEqual(t['after abort'], 'ok 5')

    def test_the_kernel_blocks_a_commit_while_a_reader_is_open_in_the_save_area(self):
        code = PRE + """
const SAV = '/sd/OS/SAVES/FT/';
fs.write(SAV + 'A.SAV', 'v1');
const r = fs.open(SAV + 'A.SAV');
const w = fs.createWriter(SAV + 'A.SAV'); w.write('v2');
E('commit while reading', () => w.commit());
T('still staged', w.size);
r.close();
E('retry', () => w.commit());
T('now', fs.readFile(SAV + 'A.SAV'));
sys.exit(0);
"""
        t, _, _ = run_ok(self, code)
        self.assertTrue(t['commit while reading'].startswith('EBUSY'), t['commit while reading']); self.assertEqual(t['still staged'], '2')
        self.assertEqual(t['retry'], 'ok 2'); self.assertEqual(t['now'], 'v2')

    def test_kernel_caps_and_full_card_map_to_errors(self):
        code = PRE + "for (let i = 0; i < 34; i++) { try { fs.write('/sd/OS/SAVES/FT/F' + i + '.SAV', 'x'); } catch (e) { T('f' + i, e.code); } } sys.exit(0);"
        t, _, d = run_ok(self, code, args=['--write-quota', '100000'])
        self.assertEqual({k: v for k, v in t.items()}, {'f32': 'EDQUOT', 'f33': 'EDQUOT'}, 'the 33rd file is refused (32 per game)')
        code = PRE + "E('new', () => fs.write('/sd/OS/SAVES/FT/A.SAV', 'x')); sys.exit(0);"
        t, _, d = run_ok(self, code, dirs=['OS/SAVES/G%02d' % i for i in range(64)])
        self.assertTrue(t['new'].startswith('EDQUOT'), '65th game folder: ' + t['new']); self.assertIn('64 game folders', t['new'])
        t, _, d = run_ok(self, code, args=['--disk-full'])
        self.assertTrue(t['new'].startswith('ENOSPC'), t['new']); self.assertFalse((d / 'OS/SAVES').exists())


class Toctou(unittest.TestCase):
    """Review 2026-11-13: readInto took the buffer's address, then ran the app's valueOf() for offset and length; a valueOf that
    detached the buffer made the read write into freed memory. The buffer is looked at again after the arguments."""

    def test_valueof_cannot_detach_the_target(self):
        code = PRE + """
const f = fs.open('/sd/OS/APPS/FT/BIN.DAT');
const b = new ArrayBuffer(64), u = new Uint8Array(b);
E('off', () => f.readInto(u, { valueOf() { b.transfer(); return 0; } }));
const b2 = new ArrayBuffer(64), u2 = new Uint8Array(b2);
E('len', () => f.readInto(u2, 0, { valueOf() { b2.transfer(); return 32; } }));
T('pos', f.position); f.close(); sys.exit(0);
"""
        t, _, _ = run_ok(self, code, files={'OS/APPS/FT/BIN.DAT': BIN})
        self.assertTrue(t['off'].startswith('TypeError'), t)
        self.assertTrue(t['len'].startswith('TypeError'), t)
        self.assertEqual(t['pos'], '0')


class Leaks(unittest.TestCase):
    def test_fifty_open_close_and_write_cycles_leave_nothing(self):
        code = PRE + """
const SAV = '/sd/OS/SAVES/FT/';
for (let i = 0; i < 50; i++) {
  const f = fs.open('/sd/OS/APPS/FT/BIN.DAT'); f.read(10); f.seek(5); f.close();
  const w = fs.createWriter(SAV + 'A.SAV'); w.write('x' + i); if (i % 2) w.commit(); else w.abort();
}
T('final', fs.readFile(SAV + 'A.SAV'));
sys.exit(0);
"""
        t, rest, _ = run_ok(self, code, files={'OS/APPS/FT/BIN.DAT': BIN})
        self.assertEqual(t['final'], 'x49'); self.assertFalse([l for l in rest if 'left' in l], rest)

    def test_fifty_app_starts_with_the_file_api_flat_in_memory(self):
        d = pathlib.Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, d, True)
        (d / 'OS/APPS/FT').mkdir(parents=True)
        (d / 'OS/APPS/FT/APP.JSON').write_text(json.dumps({'name': 'F', 'api': 1, 'capabilities': ['fs.read', 'fs.write.saves']}))
        (d / 'OS/APPS/FT/BIN.DAT').write_bytes(BIN)
        (d / 'OS/APPS/FT/MAIN.JS').write_text("""
const f = fs.open('/sd/OS/APPS/FT/BIN.DAT'); f.read(); f.close();
fs.write('/sd/OS/SAVES/FT/A.SAV', new Uint8Array(1000));
globalThis.leak = fs.createWriter('/sd/OS/SAVES/FT/B.SAV'); leak.write('pending');
globalThis.leak2 = fs.open('/sd/OS/APPS/FT/BIN.DAT');
""")
        p = subprocess.run([str(RUN), str(d), APP, '--cycles', '50', '--ms', '100', '--vclock'], capture_output=True, text=True, timeout=300)
        m = re.search(r'CYCLES (\d+) blocks first (\d+) last (\d+) leaks (\d+) open (\d+)', p.stdout)
        self.assertTrue(m, p.stdout[-300:] + p.stderr[-300:])
        n, first, last, leaks, opened = map(int, m.groups())
        self.assertEqual((n, leaks, opened), (50, 100, 0), 'each start leaves a writer and a reader; stop releases both')
        self.assertEqual(first, last, 'live heap blocks grew')
        self.assertFalse((d / 'OS/SAVES/FT/B.SAV').exists(), 'the pending writer never reached the card')
        self.assertEqual((d / 'OS/SAVES/FT/A.SAV').stat().st_size, 1000)


if __name__ == '__main__':
    RUN = pathlib.Path(sys.argv.pop(1)).resolve()
    unittest.main()
