"""Host AUDIO.TXT format, independent of MMIO and card writes."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioDiagnostics(unittest.TestCase):
    def test_format_and_bounds(self):
        with tempfile.TemporaryDirectory(prefix='audio-diag-') as directory:
            exe = str(Path(directory) / 'audio-diag')
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Iinclude', 'core/audio_diag.c', 'tests/audio_diag.c', '-o', exe],
                           cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=10)


class AudioRotation(unittest.TestCase):
    def test_rotation_and_link_without_display(self):
        with tempfile.TemporaryDirectory(prefix='audio-rot-') as directory:
            exe = str(Path(directory) / 'audio-rot')
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Iinclude', 'core/audio_rot.c', 'tests/audio_rot.c', '-o', exe],
                           cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=10)
