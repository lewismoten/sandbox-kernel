# SPDX-License-Identifier: MIT
"""run586 witnesses against exact production function bodies; fake DMA, no hardware."""
from pathlib import Path
import subprocess
import tempfile
import unittest
import ctypes as C
import test_m1_user as m1

ROOT = Path(__file__).resolve().parents[1]


class ToneSession(m1.ProtocolBase):
    def test_pre_info_tone_has_no_side_effect(self):
        cb = C.CFUNCTYPE(C.c_int, C.c_uint32, C.c_uint32)
        calls = []

        @cb
        def tone(hz, ms):
            calls.append((hz, ms))
            return 0

        class HW(C.Structure):
            _fields_ = [(n, C.c_void_p) for n in ('task', 'tone', 'report')]

        hw = HW.in_dll(self.c, 'audio_hw')
        hw.tone = C.cast(tone, C.c_void_p).value
        try:
            for command in (b'TONE 440 100\n', b'TONE invalid\n', b'AUDIO\n'):
                self.c.feed(command, len(command))
                self.assertEqual(self.run_protocol(5).strip(), 'ERR not-ready')
                self.assertEqual(calls, [])
            self.c.feed(b'INFO\n', 5)
            self.assertTrue(self.run_protocol(5).startswith('INFO 2 '))
            self.c.feed(b'TONE 440 100\n', 13)
            self.assertEqual(self.run_protocol(5).strip(), 'OK tone')
            self.assertEqual(calls, [(440, 100)])
            self.c.protocol_reset()
            self.c.feed(b'TONE 440 100\n', 13)
            self.assertEqual(self.run_protocol(5).strip(), 'ERR not-ready')
            self.assertEqual(calls, [(440, 100)])
        finally:
            hw.tone = None


class HdmiBackend(unittest.TestCase):
    def test_fill_and_dma_progress(self):
        source = (ROOT / 'drivers/hdmi_audio.c').read_text()
        # Exercise the production reader, circular builder, IRQ and task bodies.
        bodies = []
        for signature in ('static void read_pcm(', 'static void fill(', 'static void build_ring(',
                          'void hdmi_audio_irq(', 'void hdmi_audio_task('):
            start = source.index(signature)
            end = source.index('\n}', start) + 2
            bodies.append(source[start:end])
        with tempfile.TemporaryDirectory(prefix='hdmi-backend-') as d:
            d = Path(d)
            (d / 'hdmi-functions.h').write_text('\n'.join(bodies))
            exe = d / 'backend'
            subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Iinclude', '-I' + str(d), 'tests/hdmi_backend.c',
                            'core/audio_ring.c', 'core/audio_null.c', 'core/mixer.c',
                            'core/iec958.c', 'core/hdmi_audio_plan.c', '-o', str(exe)],
                           cwd=ROOT, check=True)
            subprocess.run([str(exe)], check=True, timeout=60)
