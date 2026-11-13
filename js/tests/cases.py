# SPDX-License-Identifier: MIT
"""The host cases for the JS runtime: which app runs, for how long, with which keys.
Shared by tests/run.py (check against tests/fixtures.json) and tools/make_fixtures.py
(record the fixtures from a reference jsrun built from gamemaker-os)."""
import pathlib, shutil, subprocess, tempfile, zlib

JS = pathlib.Path(__file__).resolve().parents[1]
APPS = ['HELLO', 'BENCH', 'CLOCK', 'PROBE']

# name -> jsrun arguments after the card folder (--vclock and --shot are added)
CASES = {
    'hello': ['/sd/OS/APPS/HELLO', '--ms', '1500', '--keys', '500:28:1,520:28:0'],
    'hello-keys': ['/sd/OS/APPS/HELLO', '--ms', '3000', '--keys', '300:106:1,310:106:0,600:108:1,610:108:0,900:28:1,910:28:0,1500:1:1,1510:1:0'],
    'bench': ['/sd/OS/APPS/BENCH', '--ms', '40000'],
    'clock': ['/sd/OS/APPS/CLOCK', '--ms', '2000'],
    'probe': ['/sd/OS/APPS/PROBE', '--ms', '2000', '--keys', '100:106:1,110:106:0,200:28:1,210:28:0,300:106:1,310:106:0,400:105:1,410:105:0'],
    'picker': ['--picker', '--launcher', '--ms', '1500'],
}


def card():
    """A card folder with /OS/APPS/<app> for each app, and no /GAMES (the game profile is off)."""
    d = pathlib.Path(tempfile.mkdtemp())
    (d / 'OS/APPS').mkdir(parents=True)
    for a in APPS:
        (d / 'OS/APPS' / a).symlink_to(JS / 'apps' / a)
    return d


def run(jsrun, name):
    """Runs one case; returns {rc, out (log lines), shot: {w, h, depth, crc32}}."""
    d = card()
    try:
        shot = d / 'a.shot'
        p = subprocess.run([str(jsrun), str(d)] + CASES[name] + ['--vclock', '--shot', str(shot)], capture_output=True, text=True, timeout=300)
        raw = shot.read_bytes() if shot.exists() else b''
    finally:
        shutil.rmtree(d, ignore_errors=True)
    w, h, depth = (int.from_bytes(raw[4 + 4 * i:8 + 4 * i], 'little') for i in range(3)) if raw else (0, 0, 0)
    return {'rc': p.returncode, 'out': (p.stdout + p.stderr).splitlines(), 'shot': {'w': w, 'h': h, 'depth': depth, 'crc32': zlib.crc32(raw), 'bytes': len(raw)}}
