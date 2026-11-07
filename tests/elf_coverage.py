# SPDX-License-Identifier: MIT
"""Instrument unmodified ELF/uspace code and persist gcov plus campaign evidence.

Run from any directory: python3 tests/elf_coverage.py [--deterministic]
Requires matching GCC/gcov; normal make test needs no coverage tools.
"""
import argparse
import gzip
import json
import os
import pathlib

import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--deterministic', action='store_true', help='exclude random campaigns')
    parser.add_argument('--output', type=pathlib.Path, default=ROOT / 'build/elf-coverage')
    parser.add_argument('--timeout', type=int, default=120, help='test subprocess time budget in seconds')
    args = parser.parse_args()
    output = args.output.resolve()
    # Each run gets a new directory: stale counters can never inflate coverage.
    output.mkdir(parents=True, exist_ok=True)
    import tempfile
    work = pathlib.Path(tempfile.mkdtemp(prefix='run-', dir=output))
    objects = []
    for name in ('elf', 'uspace', 'util'):
        obj = work / (name + '.o')
        subprocess.run(['cc', '-c', '-fPIC', '-O0', '-g', '--coverage', '-fno-builtin',
                        '-DHOST_TEST', '-Wall', '-Wextra', '-Werror', '-I' + str(ROOT / 'include'),
                        str(ROOT / 'core' / (name + '.c')), '-o', str(obj)], check=True)
        objects.append(str(obj))
    library = work / 'elf.so'
    subprocess.run(['cc', '-shared', '--coverage', *objects, '-o', str(library)], check=True)
    env = dict(os.environ, ELF_TEST_LIBRARY=str(library), ELF_FAILURE_DIR=str(work / 'failures'),
               ELF_FUZZ_TRACE=str(work / 'fuzz-trace.jsonl'))
    command = [sys.executable, '-m', 'unittest', 'discover', '-s', 'tests', '-p', 'test_elf_acceptance.py', '-v']
    if args.deterministic:
        command = [sys.executable, '-c',
                   'import sys,unittest;sys.path.insert(0,"tests");import test_elf_acceptance as e;'
                   'suite=unittest.defaultTestLoader.loadTestsFromTestCase(e.ElfAcceptance);'
                   'suite=unittest.TestSuite(t for t in suite if "fuzz" not in t.id());'
                   'sys.exit(not unittest.TextTestRunner(verbosity=2).run(suite).wasSuccessful())']
    start = time.monotonic()
    with (work / 'tests.log').open('w') as log:
        result = subprocess.run(command, cwd=ROOT, env=env, stdout=log, stderr=subprocess.STDOUT,
                                timeout=args.timeout)
    elapsed = time.monotonic() - start
    print((work / 'tests.log').read_text(), end='')
    gcov = subprocess.run(['gcov', '-b', '-c', '-a', '-j', str(work / 'elf.gcno')],
                          cwd=work, text=True, capture_output=True, check=True)
    (work / 'gcov.log').write_text(gcov.stdout + gcov.stderr)
    subprocess.run(['gcov', '-b', '-c', '-a', str(work / 'elf.gcno')],
                   cwd=work, text=True, capture_output=True, check=True)
    with gzip.open(work / 'elf.gcov.json.gz', 'rt') as f:
        data = json.load(f)
    elf = next(f for f in data['files'] if pathlib.Path(f['file']).name == 'elf.c')
    lines = elf['lines']
    branches = [b for line in lines for b in line['branches']]
    missed = [{'line': line['line_number'], 'branch': i, 'fallthrough': b['fallthrough']}
              for line in lines for i, b in enumerate(line['branches']) if not b['count']]
    campaigns = [json.loads(line.split('ELF_CAMPAIGN ', 1)[1])
                 for line in (work / 'tests.log').read_text().splitlines() if 'ELF_CAMPAIGN ' in line]
    summary = dict(test_exit=result.returncode, seconds=round(elapsed, 3),
                   deterministic=args.deterministic, campaigns=campaigns,
                   executable_lines=len(lines), lines_hit=sum(line['count'] > 0 for line in lines),
                   branch_outcomes=len(branches), branch_outcomes_hit=sum(b['count'] > 0 for b in branches),
                   missed_branch_outcomes=missed,
                   compiler=subprocess.check_output(['cc', '--version'], text=True).splitlines()[0],
                   gcov_version=subprocess.check_output(['gcov', '--version'], text=True).splitlines()[0],
                   baseline=subprocess.check_output(['git', 'rev-parse', 'HEAD'], cwd=ROOT, text=True).strip())
    (work / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary, indent=2))
    print('Evidence directory: ' + str(work))
    return result.returncode


if __name__ == '__main__':
    sys.exit(main())
