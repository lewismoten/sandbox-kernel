"""Real unlinked PCM core, native assertions; not syscalls or HDMI."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AudioCoreTests(unittest.TestCase):
    def test_real_unlinked_core(self):
        with tempfile.TemporaryDirectory(prefix='audio-core-') as d:
            exe = str(Path(d) / 'audio-core')
            flags = ['-O1', '-g', '-fno-omit-frame-pointer', '-fsanitize=address,undefined',
                     '-fno-sanitize-recover=all'] if os.getenv('AUDIO_SANITIZE') else ['-O2']
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', *flags,
                            '-Iinclude', 'core/audio_core.c', 'tests/audio_core.c', '-o', exe],
                           cwd=ROOT, check=True)
            subprocess.run([exe], cwd=ROOT, check=True, timeout=30)

if __name__ == '__main__':
    unittest.main(verbosity=2)
