"""Fixed-point mixer and PC-speaker tune voice, native assertions (tests/mixer.c).
AUDIO_SANITIZE=1 adds ASan/UBSan."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AudioMixerTests(unittest.TestCase):
    def test_native_mixer(self):
        with tempfile.TemporaryDirectory(prefix='audio-mixer-') as d:
            exe = str(Path(d) / 'mixer')
            flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if os.getenv('AUDIO_SANITIZE') else ['-O2']
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', *flags, '-Iinclude',
                            'core/mixer.c', 'core/audio_ring.c', 'core/audio_null.c', 'tests/mixer.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=60, capture_output=True, text=True).stdout
            self.assertIn('mixer ok', out)

if __name__ == '__main__':
    unittest.main(verbosity=2)
