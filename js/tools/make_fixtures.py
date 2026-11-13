#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Record tests/fixtures.json from a REFERENCE jsrun: gamemaker-os's own host runner, built
from the commit named in IMPORT.md (`make HOSTDIR=<dir> <dir>/jsrun` in a clean export of it).
The fixtures are what that build logged and drew (virtual clock, a card with no /GAMES).
Usage: make_fixtures.py <reference jsrun>"""
import json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / 'tests'))
import cases

def main(argv):
    if len(argv) != 1: raise SystemExit(__doc__)
    fx = {n: cases.run(argv[0], n) for n in cases.CASES}
    (cases.JS / 'tests/fixtures.json').write_text(json.dumps(fx, indent=1) + '\n')
    for n, v in fx.items(): print(n, v['rc'], len(v['out']), 'lines', v['shot'])

if __name__ == '__main__':
    main(sys.argv[1:])
