# SPDX-License-Identifier: MIT
"""core/launcher_task.c against fakes (tests/launcher_task.c): idle USB STOP, failed-start retry, GMOS never dropped by the cap."""
import pathlib, subprocess, tempfile, unittest
ROOT = pathlib.Path(__file__).resolve().parents[1]

class LauncherTask(unittest.TestCase):
    def test_task(self):
        with tempfile.TemporaryDirectory(prefix='launcher-task-') as d:
            exe = str(pathlib.Path(d) / 'lt')
            subprocess.run(['cc', '-std=gnu11', '-O1', '-g', '-fno-builtin', '-DHOST_TEST', '-D_FORTIFY_SOURCE=0', '-Wall', '-Wextra', '-Werror',
                            '-I' + str(ROOT / 'include'), '-I' + str(ROOT / 'tests/stubs'),
                            *[str(ROOT / f'core/{n}.c') for n in ('launcher_task', 'launcher', 'font', 'util', 'divide')],
                            str(ROOT / 'tests/launcher_task.c'), '-o', exe], check=True)
            out = subprocess.run([exe], check=True, timeout=30, capture_output=True, text=True).stdout
            self.assertEqual(out.count('PASS'), 3)

if __name__ == '__main__':
    unittest.main(verbosity=2)
