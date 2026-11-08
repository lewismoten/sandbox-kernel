# SPDX-License-Identifier: MIT
"""OS autostart (core/autostart.c): which program starts 10 s after boot
(/OS/AUTORUN.TXT's choice, else GMOS.ELF, else DIAG.ELF), a rejected choice
giving way to DIAG.ELF, and the fallback to DIAG.ELF when the autostarted
program keeps faulting. Real core code over fake media (tests/harness.c)."""
import ctypes as C
import os, pathlib, sys, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
import fatgen
from test_m1_user import Work, program

class Autostart(Work):
    def setUp(self):
        super().setUp()
        c=self.c;c.autostart_pick.restype=C.c_char_p;c.autostart_run.argtypes=[C.c_char_p]
        c.autostart_name.argtypes=[C.c_char_p,C.c_uint32,C.c_char_p];c.autostart_name.restype=C.c_bool
        c.autostart_fallback.restype=C.c_bool
    def card(self,files):
        t={'MIT.TXT':(ROOT/'LICENSE').read_bytes(),'OS':files}
        disk=self.work/f'card{id(self)}.img';fatgen.build(disk,t)
        self.c.open_disk(os.fsencode(disk));self.addCleanup(self.c.close_disk);self.assertEqual(self.c.asset_mount(),0)
        self.c.fb_setup(0,True);self.c.take_log()
    def name(self,text):
        out=C.create_string_buffer(20)
        return out.value.decode() if self.c.autostart_name(text,len(text),out) else None
    def path(self): return (C.c_char*32).in_dll(self.c,'os_path').value.decode()
    def fault_twice(self):
        self.c.os_handle(5,0,0);self.c.os_handle(5,0,0);return self.c.take_log().decode()

    def test_names(self):
        good={b'GMOS.ELF':'/OS/GMOS.ELF',b'gmos.elf\r\n':'/OS/GMOS.ELF',b'  DIAG.ELF \n# a note':'/OS/DIAG.ELF',
              b'A_B-12CD.ELF':'/OS/A_B-12CD.ELF'}
        for t,want in good.items():
            with self.subTest(t): self.assertEqual(self.name(t),want)
        for t in (b'',b'\n',b'.ELF',b'TOOLONGNM.ELF',b'GMOS.TXT',b'GMOS',b'GMOS.EL',b'../GMOS.ELF',b'/OS/GMOS.ELF',
                  b'GAMES/X.ELF',b'GMOS.ELF X',b'GM OS.ELF',b'GMOS.ELFX',b'GM\x00S.ELF',b'GMOS.ELF\x00'):
            with self.subTest(t): self.assertIsNone(self.name(t))

    def test_gmos_first_then_diag(self):
        self.card({'DIAG.ELF':program(),'GMOS.ELF':program()});self.assertEqual(self.c.autostart_pick(),b'/OS/GMOS.ELF')
    def test_diag_without_gmos(self):
        self.card({'DIAG.ELF':program()});self.assertEqual(self.c.autostart_pick(),b'/OS/DIAG.ELF')
    def test_autorun_choice(self):
        self.card({'DIAG.ELF':program(),'GMOS.ELF':program(),'AUTORUN.TXT':b'diag.elf\n'})
        self.assertEqual(self.c.autostart_pick(),b'/OS/DIAG.ELF');self.assertIn(b'choice from /OS/AUTORUN.TXT',self.c.take_log())
    def test_autorun_ignored(self):
        for text in (b'MISSING.ELF\n',b'../KERNEL.IMG',b'GMOS.ELF and more',b'\xff'*64):
            with self.subTest(text):
                self.card({'DIAG.ELF':program(),'GMOS.ELF':program(),'AUTORUN.TXT':text})
                self.assertEqual(self.c.autostart_pick(),b'/OS/GMOS.ELF');self.assertIn(b'ignored',self.c.take_log())
                # Each subcase owns a different card, but the C harness has
                # one current FILE*. Close it before opening the next one.
                self.doCleanups()
    def test_nothing_on_card(self):
        self.card({'OTHER.ELF':program()})
        self.assertEqual(self.c.autostart_pick(),b'/OS/DIAG.ELF');self.assertNotEqual(self.c.autostart_run(b'/OS/DIAG.ELF'),0)
        self.assertFalse(self.c.os_running())

    def test_rejected_choice_runs_diag(self):
        self.card({'DIAG.ELF':program(),'GMOS.ELF':b'not an elf'})
        self.assertEqual(self.c.autostart_run(self.c.autostart_pick()),0);log=self.c.take_log()
        self.assertIn(b'OS reject /OS/GMOS.ELF',log);self.assertIn(b'OS start /OS/DIAG.ELF',log);self.assertEqual(self.path(),'/OS/DIAG.ELF')
    def test_repeated_faults_fall_back_to_diag(self):
        self.card({'DIAG.ELF':program(),'GMOS.ELF':program()})
        self.assertEqual(self.c.autostart_run(self.c.autostart_pick()),0);self.assertEqual(self.path(),'/OS/GMOS.ELF')
        self.c.os_handle(5,0,0);self.assertIn(b'OS restart',self.c.take_log());self.assertEqual(self.path(),'/OS/GMOS.ELF')
        self.c.advance(5_000_000);self.c.os_handle(5,0,0);log=self.c.take_log().decode()
        self.assertIn('OS start /OS/DIAG.ELF',log)
        self.assertNotIn('recovery console kept',log);self.assertTrue(self.c.os_running());self.assertEqual(self.path(),'/OS/DIAG.ELF')
        self.assertEqual(C.c_char_p.in_dll(self.c,'last_error').value,b'OS fell back to DIAG.ELF after repeated faults')   # error(): the log and the status panel (the host harness only records it)
        self.assertIn('kernel recovery console kept',self.fault_twice())   # DIAG itself faulting: the console, no loop
        self.assertFalse(self.c.os_running())
    def test_diag_autostart_has_no_fallback(self):
        self.card({'DIAG.ELF':program()})
        self.assertEqual(self.c.autostart_run(self.c.autostart_pick()),0)
        log=self.fault_twice();self.assertIn('kernel recovery console kept',log);self.assertNotIn('fell back',log)
    def test_usb_run_cancels_fallback(self):
        self.card({'DIAG.ELF':program(),'GMOS.ELF':program()})
        self.assertEqual(self.c.autostart_run(b'/OS/GMOS.ELF'),0);self.c.autostart_cancel()
        log=self.fault_twice();self.assertIn('kernel recovery console kept',log);self.assertNotIn('fell back',log)
    def test_clean_exit_keeps_console(self):
        self.card({'DIAG.ELF':program(),'GMOS.ELF':program()})
        self.assertEqual(self.c.autostart_run(b'/OS/GMOS.ELF'),0);self.c.take_log()
        f=(C.c_uint32*17).in_dll(self.c,'os_frame');f[15]=0x0000f000;f[0]=0;self.c.os_handle(4,0,0x0000f000)
        log=self.c.take_log();self.assertIn(b'OS exit 0 /OS/GMOS.ELF; recovery console',log);self.assertFalse(self.c.os_running())

if __name__=='__main__': unittest.main()
