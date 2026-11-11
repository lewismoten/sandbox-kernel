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
    def test_audio_initializes_before_cec(self):
        source = (ROOT / 'core/main.c').read_text()
        self.assertLess(source.index('fb_start();'), source.index('hdmi_audio_init();'))
        self.assertLess(source.index('hdmi_audio_init();'), source.index('cec_hw_init();'))

    def test_boot_replay_keeps_audio_result(self):
        source = (ROOT / 'core/probe.c').read_text()
        source = source[source.index('static char notes['):source.index('void asset_probe(')]
        harness = r'''
#include <assert.h>
#include <string.h>
static char last[256]; static unsigned lines, cec_reports;
static void log_line(const char *line) { assert(strlen(line) < sizeof last); strcpy(last, line); lines++; }
static void append(char **out, const char *text) { while (*text) *(*out)++ = *text++; **out = 0; }
static void cec_hw_report(void) { cec_reports++; }
'''
        harness += source + r'''
int main(void) {
 const char *result = "AUDIO hdmi: DMA draining; HDMI replaces the null backend (silence until /dev/audio or TONE)";
 for (unsigned index = 0; index < 40; index++) boot_note("setup");
 boot_note(result); lines = 0; boot_summary();
 assert(lines == 41 && !strncmp(last, "BOOT ", 5) && !strcmp(last + 5, result));
 assert(cec_reports == 1);
 char long_line[256]; memset(long_line, 'x', sizeof long_line - 1); long_line[255] = 0;
 for (unsigned index = 0; index < 40; index++) boot_note(long_line);
 lines = 0; boot_summary(); assert(lines == 64 && strlen(last) == 132);
 assert(cec_reports == 2);
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='hdmi-boot-notes-') as directory:
            source_file = Path(directory) / 'notes.c'
            executable = Path(directory) / 'notes'
            source_file.write_text(harness)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            str(source_file), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True, timeout=10)

    def test_startup_register_sequence(self):
        source = (ROOT / 'drivers/hdmi_audio.c').read_text()
        for signature in ('static uint32_t irq_save(', 'static void irq_restore('):
            start = source.index(signature)
            end = source.index('\n}', start) + 2
            source = source[:start] + source[end:]
        helpers = ('static uint32_t bus(', 'static void drain(', 'static void hd(',
                   'static void hc(', 'static uint32_t dma(', 'static void dmaw(')
        source = '\n'.join(line for line in source.splitlines()
                           if line != '#include "kernel.h"' and not line.startswith(helpers))
        with tempfile.TemporaryDirectory(prefix='hdmi-startup-') as directory:
            directory = Path(directory)
            (directory / 'hdmi-driver.h').write_text(source)
            executable = directory / 'startup'
            subprocess.run(['cc', '-std=gnu11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-Iinclude', '-I' + str(directory), 'tests/hdmi_startup.c',
                            'core/audio_ring.c', 'core/audio_null.c', 'core/mixer.c',
                            'core/iec958.c', 'core/hdmi_audio_plan.c', 'core/audio_diag.c', '-o', str(executable)],
                           cwd=ROOT, check=True)
            subprocess.run([str(executable)], check=True, timeout=60)

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
