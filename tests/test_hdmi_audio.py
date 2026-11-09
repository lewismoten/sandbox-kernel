"""HDMI audio planning (clocks, N/CTS, infoframe, DMA control-block chain), host-native."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]

class HdmiAudioTests(unittest.TestCase):
    def test_native(self):
        with tempfile.TemporaryDirectory(prefix='hdmi-audio-') as d:
            exe = str(Path(d) / 'hdmi_audio')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                            '-Iinclude', 'core/iec958.c', 'core/hdmi_audio_plan.c', 'tests/hdmi_audio.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=60, capture_output=True, text=True).stdout
            self.assertIn('hdmi audio ok', out)

if __name__ == '__main__':
    unittest.main(verbosity=2)
