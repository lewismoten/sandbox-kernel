#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Slice 7 tests: apps.list / apps.launch / apps.exit (gap G5). A JS app starts another from inside JS.ELF; the runtime stops the
caller (every handle released), starts the target, passes sandbox.launchArgs and returns on apps.exit. Covered: list with good and bad
manifests, a launch and exit round trip with args, the launch stack's depth limit, a failed launch that returns to the caller, handles
released between apps, 50 launch/exit cycles with zero leaks and a flat heap, capability denied, and exit with nobody to return to.
Usage: launch_run.py <jsrun>"""
import json, pathlib, shutil, subprocess, sys, tempfile, unittest

RUN = None


def manifest(caps=('apps.launch',), **kw):
    m = {'name': 'T', 'version': '1.0.0', 'api': 1, 'capabilities': list(caps)}
    m.update(kw)
    return json.dumps(m)


def app(code, caps=('apps.launch',), **kw):
    return {'APP.JSON': manifest(caps, **kw), 'MAIN.JS': code}


def go(apps, autorun, ms=3000, extra=()):
    """Runs the card from AUTORUN (APPS/NAME); returns (rc, log lines with 'JS ' stripped)."""
    d = pathlib.Path(tempfile.mkdtemp())
    try:
        for name, files in apps.items():
            (d / 'OS/APPS' / name).mkdir(parents=True)
            for f, t in files.items():
                (d / 'OS/APPS' / name / f).write_text(t)
        (d / 'OS').mkdir(exist_ok=True)
        (d / 'OS/AUTORUN.TXT').write_text('JS.ELF\nAPPS/%s\n' % autorun)
        p = subprocess.run([str(RUN), str(d), 'x', '--autorun', '--vclock', '--ms', str(ms)] + list(extra), capture_output=True, text=True, timeout=300)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    return p.returncode, [l[3:] if l.startswith('JS ') else l for l in (p.stdout + p.stderr).splitlines()]


def t(lines):
    return [l[2:] for l in lines if l.startswith('T ')]


class Launch(unittest.TestCase):
    def test_list_with_good_and_bad_manifests(self):
        apps = {
            'LIST': app("for (const a of apps.list()) console.log('T ' + [a.id, a.name, a.version, a.api, a.ok, a.reason, a.capabilities.join(',')].join('|')); sys.exit(0)"),
            'GOOD': app("", ('display', 'fs.read'), name='Good One', version='2.1', description='x'),
            'BADJSON': {'APP.JSON': '{"name": "x"', 'MAIN.JS': ''},
            'NEWAPI': app("", (), api=99),
            'NOENTRY': {'APP.JSON': manifest(()), },
            'OLD': {'MAIN.JS': '// legacy'},
            'EMPTY': {'README.TXT': 'not an app'},
            'ODD.NAME': app(""),
        }
        _, lines = go(apps, 'LIST')
        got = {}
        for l in t(lines):
            i, name, version, api, ok, reason, caps = l.split('|')
            got[i] = {'id': i, 'name': name, 'version': version, 'api': int(api), 'ok': ok == 'true', 'reason': reason, 'capabilities': caps.split(',') if caps else []}
        self.assertEqual(sorted(got), ['BADJSON', 'GOOD', 'LIST', 'NEWAPI', 'NOENTRY', 'OLD'])   # EMPTY is not an app; ODD.NAME is not an id
        g = got['GOOD']
        self.assertEqual((g['name'], g['version'], g['api'], g['ok'], g['reason']), ('Good One', '2.1', 1, True, ''))
        self.assertEqual(g['capabilities'], ['log', 'time', 'memory', 'app.lifecycle', 'fs.read', 'display'])
        self.assertFalse(got['BADJSON']['ok']); self.assertTrue(got['BADJSON']['reason'])
        self.assertEqual(got['BADJSON']['name'], 'BADJSON')
        self.assertIn('needs runtime API 99', got['NEWAPI']['reason'])
        self.assertEqual((got['NOENTRY']['ok'], got['NOENTRY']['reason']), (False, 'MAIN.JS is not in the app folder'))
        self.assertTrue(got['OLD']['ok']); self.assertEqual(got['OLD']['name'], 'OLD')

    def test_round_trip_passes_args_and_the_exit_code(self):
        apps = {
            'A': app("""if (sandbox.returned) { console.log('T back ' + JSON.stringify(sandbox.returned) + ' ' + sandbox.launchArgs); sys.exit(0); }
                        else { console.log('T A start'); apps.launch('B', {args: {n: 7, s: 'hi'}}); }"""),
            'B': app("""console.log('T B args ' + JSON.stringify(sandbox.launchArgs) + ' ' + sandbox.app.id + ' depth ' + sandbox.launchDepth); apps.exit(3);"""),
        }
        rc, lines = go(apps, 'A')
        self.assertEqual(t(lines), ['A start', 'B args {"n":7,"s":"hi"} B depth 1', 'back {"id":"B","code":3} undefined'])
        self.assertEqual(rc, 0)
        self.assertNotIn('Uncaught', ' '.join(lines))

    def test_launch_stack_depth_limit_and_unwinding(self):
        apps = {'R': app("""const n = sandbox.launchArgs ? sandbox.launchArgs.n : 0;
            if (sandbox.returned) { if (sandbox.launchArgs) apps.exit(sandbox.returned.code + 1); else { console.log('T root got ' + sandbox.returned.code); sys.exit(0); } }
            else try { apps.launch('R', {args: {n: n + 1}}); } catch (e) { console.log('T full at ' + n + ' ' + e.constructor.name + ' ' + e.message); apps.exit(10); }""")}
        rc, lines = go(apps, 'R')
        out = t(lines)
        self.assertEqual(out[0].split(' ', 3)[:3], ['full', 'at', '4'])
        self.assertIn('RangeError', out[0])
        self.assertEqual(out[1], 'root got 13')   # 10 from depth 4, +1 for each of the three callers on the way back

    def test_failed_launch_returns_to_the_caller_with_the_reason(self):
        apps = {
            'A': app("""if (sandbox.launchError) { console.log('T failed ' + JSON.stringify(sandbox.launchError)); apps.on('launcherror', e => { console.log('T event ' + e.id); sys.exit(0); }); }
                        else apps.launch(sandbox.launchArgs ? 'x' : 'WHICH'.replace('WHICH', 'BADJSON'))"""),
            'BADJSON': {'APP.JSON': '{"name": "x"', 'MAIN.JS': 'console.log("T BAD RAN")'},
        }
        rc, lines = go(apps, 'A')
        self.assertEqual(rc, 0)
        out = t(lines)
        self.assertEqual(len(out), 2, lines); self.assertTrue(out[0].startswith('failed {"id":"BADJSON","reason":"'), out); self.assertEqual(out[1], 'event BADJSON')
        for name, files, why in (
                ('NEWAPI', app("console.log('T RAN')", (), api=99), 'needs runtime API 99'),
                ('NOENTRY', {'APP.JSON': manifest(())}, 'MAIN.JS is not in the app folder'),
                ('GONE', None, 'MAIN.JS is not in the app folder'),
                ('THROWS', app("throw new Error('boom')"), 'threw an error')):
            a = {'A': app("""if (sandbox.launchError) { console.log('T ' + sandbox.launchError.id + ' | ' + sandbox.launchError.reason); sys.exit(0); } else apps.launch('%s')""" % name)}
            if files: a[name] = files
            rc, lines = go(a, 'A')
            self.assertEqual(rc, 0, lines)
            self.assertEqual(len(t(lines)), 1, lines)
            self.assertTrue(t(lines)[0].startswith(name + ' | ') and why in t(lines)[0], (name, lines))
            self.assertTrue(any('launch of %s failed' % name in l for l in lines), lines)

    def test_handles_are_released_between_apps(self):
        apps = {
            'A': app("""if (sandbox.returned) sys.exit(0);
                        else { globalThis.f = fs.open('/sd/OS/APPS/A/MAIN.JS'); globalThis.q = input.open(); apps.launch('B'); }""", ('fs.read', 'input', 'apps.launch')),
            'B': app("apps.exit(0)", ('fs.read', 'input', 'apps.launch')),
        }
        rc, lines = go(apps, 'A', extra=['--blocks'])
        self.assertEqual(rc, 0)
        self.assertTrue(any('left 2 handle(s) open' in l for l in lines), lines)   # A's two, closed by the runtime
        self.assertTrue(all(l.endswith('open 0') for l in lines if l.startswith('HOST blocks')), lines)
        self.assertEqual(len([l for l in lines if l.startswith('HOST blocks')]), 3)

    def test_fifty_cycles_no_leaks_flat_heap(self):
        apps = {
            'A': app("""const n = sandbox.returned ? sandbox.returned.code : 0;
                        const f = fs.open('/sd/OS/APPS/A/MAIN.JS'); const x = new Uint8Array(4096); f.readInto(x, 0); f.close();
                        if (n >= 50) { console.log('T cycles ' + n); sys.exit(0); } else apps.launch('B', {args: {n}});""", ('fs.read', 'apps.launch')),
            'B': app("""const q = input.open(); q.close(); apps.exit(sandbox.launchArgs.n + 1)""", ('input', 'apps.launch')),
        }
        rc, lines = go(apps, 'A', ms=60000, extra=['--blocks'])
        self.assertEqual(rc, 0)
        self.assertEqual(t(lines), ['cycles 50'])
        self.assertFalse([l for l in lines if 'left ' in l and 'open' in l], 'a handle was left open')
        blocks = [l.split() for l in lines if l.startswith('HOST blocks')]
        self.assertEqual(len(blocks), 101)   # A and B 50 times each, then A once more
        self.assertTrue(all(b[4] == '0' for b in blocks))
        a_stops = [int(b[2]) for b in blocks[0::2]]   # after each stop of A
        b_stops = [int(b[2]) for b in blocks[1::2]]
        self.assertEqual(a_stops[2], a_stops[-2], 'live heap blocks grew')
        self.assertEqual(b_stops[2], b_stops[-1], 'live heap blocks grew')

    def test_capability_is_required(self):
        apps = {'A': app("""for (const k of ['list', 'exit']) { try { apps[k](); console.log('T ' + k + ' allowed'); } catch (e) { console.log('T ' + k + ' ' + e.message); } }
                            try { apps.launch('A'); console.log('T launch allowed'); } catch (e) { console.log('T launch ' + e.message); }
                            console.log('T has ' + sandbox.has('apps.launch')); sys.exit(0)""", ('display',))}
        rc, lines = go(apps, 'A')
        out = t(lines)
        self.assertEqual(out[0], "list apps.list: capability 'apps.launch' is not granted to this app")
        self.assertEqual(out[1], "exit apps.exit: capability 'apps.launch' is not granted to this app")
        self.assertEqual(out[2], "launch apps.launch: capability 'apps.launch' is not granted to this app")
        self.assertEqual(out[3], 'has false')
        self.assertFalse([l for l in lines if 'JS launch' in l or 'launch A from' in l])

    def test_exit_with_nobody_to_return_to_goes_to_the_picker(self):
        _, lines = go({'A': app("console.log('T A'); apps.exit(0)")}, 'A', ms=1500)
        self.assertEqual(t(lines), ['A'])
        self.assertTrue(any(l.startswith('app picker') for l in lines), lines)   # A is the AUTORUN app, so there is nothing else to return to

    def test_launched_app_returns_to_autorun_app_when_launched_from_the_picker_path(self):
        # exit from an app nobody launched, while another app is AUTORUN: back to AUTORUN
        apps = {'H': app("""console.log('T H ' + (sandbox.returned ? sandbox.returned.id : 'first')); if (sandbox.returned) sys.exit(0); else apps.launch('X')"""),
                'X': app("apps.exit(0)")}
        _, lines = go(apps, 'H')
        self.assertEqual(t(lines), ['H first', 'H X'])

    def test_manifest_validator_knows_the_capability(self):
        sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tools'))
        import app_check
        ok, _ = app_check.check_manifest(manifest().encode())
        self.assertTrue(ok)
        self.assertFalse(app_check.check_manifest(manifest(('apps.launchx',)).encode())[0])


if __name__ == '__main__':
    RUN = sys.argv.pop(1)
    unittest.main()
