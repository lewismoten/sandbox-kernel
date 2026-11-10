"""Real audio ring and null backend, native assertions plus a two-thread
SPSC stress. AUDIO_SANITIZE=1 adds ASan/UBSan. No syscalls: see test_audio_dev.py."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class AudioRingTests(unittest.TestCase):
    def test_native_ring(self):
        with tempfile.TemporaryDirectory(prefix='audio-ring-') as d:
            exe = str(Path(d) / 'audio-ring')
            flags = ['-O1', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all'] if os.getenv('AUDIO_SANITIZE') else ['-O2']
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', *flags, '-pthread', '-Iinclude',
                            'core/audio_ring.c', 'core/audio_null.c', 'tests/audio_ring.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=60, capture_output=True, text=True).stdout
            self.assertIn('audio ring ok', out)

if __name__ == '__main__':
    unittest.main(verbosity=2)
