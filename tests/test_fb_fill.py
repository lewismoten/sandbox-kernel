# SPDX-License-Identifier: MIT
# Hermes OpenAI Codex GPT-6.1 Sol <codex@openai.com>.
"""Real production fill against scalar pixel API and independent oracle."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def run_probe(directory, source=None, sanitize=False):
    binary = Path(directory) / 'fb-fill'
    command = ['cc', '-std=c11', '-Os', '-g', '-fno-builtin', '-DHOST_TEST',
               '-ffunction-sections', '-fdata-sections', '-Wall', '-Wextra',
               '-Werror', '-Wno-misleading-indentation', '-I' + str(ROOT / 'include')]
    if sanitize:
        command += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    # Match the repo's HOST_TEST path; Apple ld uses -dead_strip, not GNU --gc-sections.
    link = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
    command += [str(ROOT / 'tests/fb_fill_probe.c'),
                str(source or ROOT / 'core/fb.c'), link, '-o', str(binary)]
    subprocess.run(command, check=True)
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1:halt_on_error=1',
               UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1')
    return subprocess.run([str(binary)], capture_output=True, text=True, env=env)


class ProbeBuild(unittest.TestCase):
    def test_linker_selection_normal_and_sanitizer(self):
        for platform, selected, rejected in (
                ('darwin', '-Wl,-dead_strip', '-Wl,--gc-sections'),
                ('linux', '-Wl,--gc-sections', '-Wl,-dead_strip')):
            for sanitize in (False, True):
                with self.subTest(platform=platform, sanitize=sanitize):
                    with tempfile.TemporaryDirectory() as directory:
                        with patch('sys.platform', platform), patch.object(subprocess, 'run') as run:
                            run_probe(directory, sanitize=sanitize)
                    command = run.call_args_list[0].args[0]
                    self.assertIn(selected, command)
                    self.assertNotIn(rejected, command)

    def test_normal_and_sanitizer_use_host_test_path(self):
        for sanitize in (False, True):
            with self.subTest(sanitize=sanitize), tempfile.TemporaryDirectory() as directory:
                with patch.object(subprocess, 'run') as run:
                    run_probe(directory, sanitize=sanitize)
                command = run.call_args_list[0].args[0]
                self.assertIn('-DHOST_TEST', command)
                self.assertIn('-Werror', command)
                self.assertEqual('-fsanitize=address,undefined' in command, sanitize)
                self.assertFalse(any(flag.startswith('-Wno-asm') for flag in command))


class Fill(unittest.TestCase):
    def test_587_exact_surfaces(self):
        with tempfile.TemporaryDirectory() as directory:
            result = run_probe(directory)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn('PASS 587 surfaces', result.stdout)
        print(result.stdout, end='')

    def test_floor_boundary_mutation_is_rejected(self):
        source = (ROOT / 'core/fb.c').read_text()
        self.assertEqual(source.count('((k+1)*w+7)/8'), 1)
        self.assertEqual(source.count('((k+1)*w+15)/16'), 1)
        source = source.replace('((k+1)*w+7)/8', '((k+1)*w)/8')
        source = source.replace('((k+1)*w+15)/16', '((k+1)*w)/16')
        with tempfile.TemporaryDirectory() as directory:
            mutant = Path(directory) / 'floor.c'
            mutant.write_text(source)
            result = run_probe(directory, mutant)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn('MISMATCH w=17 h=33', result.stderr)
        print('EXPECTED RED floor mutation exit=1: ' + result.stderr.strip())


if __name__ == '__main__':
    if os.environ.get('FB_FILL_SANITIZE') == '1':
        with tempfile.TemporaryDirectory() as directory:
            result = run_probe(directory, sanitize=True)
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            raise SystemExit(result.returncode)
        assert 'PASS 587 surfaces' in result.stdout
    else:
        unittest.main()
