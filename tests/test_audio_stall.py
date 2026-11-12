"""HDMI audio stall detection and its AUDIO.TXT line, host-native."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AudioStallTests(unittest.TestCase):
    def test_native(self):
        with tempfile.TemporaryDirectory(prefix='audio-stall-') as d:
            exe = str(Path(d) / 'audio_stall')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            'core/audio_stall.c', 'tests/audio_stall.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=30, capture_output=True, text=True).stdout
            self.assertIn('audio stall ok', out)

    def test_stats_native(self):
        with tempfile.TemporaryDirectory(prefix='audio-stats-') as d:
            exe = str(Path(d) / 'audio_stats')
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O1', '-Iinclude',
                            'core/audio_stats.c', 'core/audio_diag.c', 'tests/audio_stats.c', '-o', exe], cwd=ROOT, check=True)
            out = subprocess.run([exe], cwd=ROOT, check=True, timeout=30, capture_output=True, text=True).stdout
            self.assertIn('audio stats ok', out)

    def test_driver_falls_back_on_stall(self):
        src = (ROOT / 'drivers/hdmi_audio.c').read_text()
        self.assertIn('audio_stall_check(&stall_watch', src)
        self.assertIn('audio_hw.task = 0', src[src.index('audio_stall_check(&stall_watch'):])


if __name__ == '__main__':
    unittest.main(verbosity=2)
