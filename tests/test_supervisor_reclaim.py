# SPDX-License-Identifier: MIT
"""Real supervisor/ELF/uspace over the existing media boundary. No ARM execution."""
import ctypes as C
from test_m1_user import CallsBase, Frame, IMAGE, program
import hostlib

class Reclaim(CallsBase):
    def card(self,extra=None,order=0,fb=True):
        super().card(extra={'GMOS.ELF':program(),**(extra or {})},order=order,fb=fb)
    @classmethod
    def setUpClass(cls):
        super().setUpClass();hostlib.build(cls.work/'core.dylib',supervisor_probe=True)
    def setUp(self):
        super().setUp()
        for name in ('probe_epoch','probe_last'): getattr(self.c,name).restype=C.c_uint64
        self.c.probe_last_set.argtypes=[C.c_uint64]
    def drain(self):
        for _ in range(4097):
            if self.c.probe_phase()!=3: return
            self.c.os_task()
        self.fail('cleanup failed to complete')
    def finish(self):
        f=Frame.in_dll(self.c,'os_frame');f.pc=0xf000;f.r[0]=0
        self.c.os_handle(4,0,0xf000);self.drain()

    def protocol(self, command):
        c=self.c;c.feed(command,len(command));c.protocol_task()
        return c.take_output()
    def autostart(self):
        self.finish();self.c.autostart_run.argtypes=[C.c_char_p]
        self.assertEqual(self.c.autostart_run(b'/OS/GMOS.ELF'),0)
        C.c_bool.in_dll(self.c,'storage_ready').value=True
        self.protocol(b'INFO\n')
    def test_protocol_busy_run_preserves_pending_restart_budget(self):
        class Policy(C.Structure):
            _fields_=[('faults',C.c_uint32),('first_us',C.c_uint64)]
        self.autostart();c=self.c;c.os_handle(5,1,IMAGE)
        policy=Policy.in_dll(c,'os_policy_state')
        self.assertEqual(policy.faults,1)
        self.assertIn(b'ERR',self.protocol(b'RUN GMOS.ELF\n'))
        self.assertEqual(policy.faults,1)
        self.drain();self.assertTrue(c.os_running())
        c.os_handle(5,1,IMAGE);self.drain()
        self.assertTrue(c.os_running())
        self.assertEqual((C.c_char*32).in_dll(c,'os_path').value,b'/OS/DIAG.ELF')

    def test_protocol_busy_run_preserves_pending_fallback(self):
        self.autostart();c=self.c
        c.os_handle(5,1,IMAGE);self.drain()
        c.os_handle(5,1,IMAGE)
        self.assertEqual(c.probe_phase(),3)
        self.assertEqual(self.protocol(b'RUN GMOS.ELF\n'),b'ERR not-ready\n')
        self.drain();self.assertTrue(c.os_running())
        self.assertEqual((C.c_char*32).in_dll(c,'os_path').value,b'/OS/DIAG.ELF')
    def test_protocol_busy_run_during_launch_does_not_cancel_autostart(self):
        self.finish();c=self.c
        C.c_bool.in_dll(c,'storage_ready').value=True
        self.protocol(b'INFO\n')
        C.c_uint32.in_dll(c,'os_policy_state').value=1
        command=b'RUN GMOS.ELF\n';c.feed(command,len(command));c.protocol_on_next_sd_read()
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
        self.assertEqual(c.take_output(),b'ERR not-ready\n')
        c.autostart_cancelled.restype=C.c_bool
        self.assertFalse(c.autostart_cancelled())
        self.assertEqual(C.c_uint32.in_dll(c,'os_policy_state').value,1)
        self.assertEqual(c.probe_last(),2)
    def test_protocol_busy_run_sticky_quarantine_preserves_policy(self):
        self.autostart();c=self.c
        (C.c_uint32*4096).in_dll(c,'us_table')[IMAGE>>20]^=0x400
        c.os_handle(5,1,IMAGE);used=c.us_used()
        c.autostart_cancelled.restype=C.c_bool
        self.assertFalse(c.os_cleanup_pending())
        self.assertEqual(self.protocol(b'RUN GMOS.ELF\n'),b'ERR not-ready\n')
        self.assertFalse(c.autostart_cancelled())
        self.assertEqual(C.c_uint32.in_dll(c,'os_policy_state').value,1)
        c.os_task();self.assertEqual(c.us_used(),used)
        self.assertEqual(c.probe_phase(),3)
    def test_protocol_stop_cancels_pending_fallback(self):
        self.autostart();c=self.c
        c.os_handle(5,1,IMAGE);self.drain();c.os_handle(5,1,IMAGE)
        self.assertEqual(self.protocol(b'STOP\n'),b'')
        c.os_task();self.drain();c.protocol_task()
        self.assertIn(b'OK stop',c.take_output());self.assertFalse(c.os_running())
        self.assertEqual(c.us_used(),0)
    def test_protocol_fresh_run_resets_budget_and_cancels_autostart(self):
        self.autostart();c=self.c;self.finish()
        C.c_uint32.in_dll(c,'os_policy_state').value=1
        self.assertEqual(self.protocol(b'RUN GMOS.ELF\n'),b'OK run /OS/GMOS.ELF\n')
        self.assertEqual(C.c_uint32.in_dll(c,'os_policy_state').value,0)
        c.autostart_cancelled.restype=C.c_bool;self.assertTrue(c.autostart_cancelled())
        c.os_handle(5,1,IMAGE);self.drain();c.os_handle(5,1,IMAGE);self.drain()
        self.assertFalse(c.os_running())
    def test_protocol_stop_pending_run_preserves_policy(self):
        self.autostart();c=self.c;self.finish()
        self.assertFalse(c.os_running());c.os_request_stop()
        C.c_uint32.in_dll(c,'os_policy_state').value=1
        self.assertEqual(self.protocol(b'RUN GMOS.ELF\n'),b'ERR not-ready\n')
        self.assertEqual(C.c_uint32.in_dll(c,'os_policy_state').value,1)
        c.autostart_cancelled.restype=C.c_bool;self.assertFalse(c.autostart_cancelled())

    def test_distinct_epoch_and_nonwrapping_exhaustion(self):
        c=self.c
        self.assertEqual(c.probe_epoch(),1);self.finish()
        self.assertEqual(c.probe_epoch(),0);self.assertEqual(c.probe_last(),1)
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
        self.assertEqual(c.probe_epoch(),2);self.finish()
        c.probe_last_set(2**64-2)
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
        self.assertEqual(c.probe_epoch(),2**64-1);self.finish()
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-75)
        self.assertEqual(c.probe_epoch(),0);self.assertFalse(c.os_running())
        self.assertEqual(c.us_used(),0)

    def test_every_partial_map_failure_consumes_epoch_and_rolls_back(self):
        self.finish();c=self.c
        for n in range(1,5):
            old=c.probe_last();c.probe_fail_map(n)
            self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-12)
            self.assertFalse(c.os_running());self.assertEqual(c.probe_epoch(),0)
            self.assertEqual(c.probe_last(),old+1)
            self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
            self.drain();self.assertEqual(c.us_used(),0)
        c.probe_fail_map(0)
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
    def test_stop_during_launch_read_cancels_entry(self):
        self.finish();c=self.c;old=c.probe_last()
        c.feed(b'INFO\n',5);c.protocol_task();c.take_output()
        c.feed(b'STOP\n',5);c.protocol_on_next_sd_read()
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
        self.assertFalse(c.os_running());self.assertEqual(c.probe_epoch(),0)
        self.assertEqual(c.probe_last(),old)
        for _ in range(4097):
            c.os_task()
            if not c.os_stop_pending(): break
        self.assertEqual(c.us_used(),0)

    def test_failed_autostart_and_restart_defer_diag_until_rollback(self):
        self.finish();c=self.c;c.autostart_run.argtypes=[C.c_char_p]
        c.probe_fail_map(3)
        self.assertEqual(c.autostart_run(b'/OS/GMOS.ELF'),-12)
        self.assertFalse(c.os_running());self.assertEqual(c.probe_epoch(),0)
        self.drain()
        self.assertTrue(c.os_running())
        self.assertEqual((C.c_char*32).in_dll(c,'os_path').value,b'/OS/DIAG.ELF')
        self.finish();c.probe_fail_map(0)
        self.assertEqual(c.autostart_run(b'/OS/GMOS.ELF'),0)
        c.probe_fail_map(4);c.os_handle(5,1,IMAGE)
        self.drain();self.assertTrue(c.os_running())
        self.assertEqual((C.c_char*32).in_dll(c,'os_path').value,b'/OS/DIAG.ELF')

    def test_stop_at_final_launch_callback_rolls_back(self):
        self.finish();c=self.c;c.fb_home();c.probe_stop_on_mode();old=c.probe_last()
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
        self.assertFalse(c.os_running());self.assertEqual(c.probe_epoch(),0)
        self.assertEqual(c.probe_last(),old+1)
        self.assertGreater(c.us_used(),0)
        for _ in range(4097):
            c.os_task()
            if not c.os_stop_pending(): break
        self.assertEqual(c.us_used(),0)
        self.assertEqual(bytes(Frame.in_dll(c,'os_frame')),bytes(C.sizeof(Frame)))

    def test_all_terminal_paths_revoke_before_any_reentry(self):
        import struct
        from test_m1_user import poke, SVC0
        c=self.c
        events=['return0','return7','exit0','exit7','exit-negative','undef','data','prefetch','unknown','thumb','svc-fetch','ready','heartbeat','sleep']
        for i,event in enumerate(events):
            with self.subTest(event=event):
                if i: self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
                used=c.us_used();last=c.probe_last();f=Frame.in_dll(c,'os_frame')
                if event.startswith('return'):
                    f.pc=0xf000;f.r[0]=int(event[6:]);c.os_handle(4,0,0)
                elif event.startswith('exit'):
                    status={'exit0':0,'exit7':7,'exit-negative':-9}[event]
                    rec=self.hdr(24)+struct.pack('<q',status);poke(c,self.REC,rec)
                    f.r[0]=self.REC;f.r[1]=24;f.r[2]=1;f.r[7]=16;f.pc=IMAGE+4;c.os_handle(1,0,0)
                elif event=='thumb': f.cpsr=0x30;c.os_handle(1,0,0)
                elif event=='svc-fetch': f.pc=4;c.os_handle(1,0,0)
                elif event=='ready': c.advance(10_000_000);c.os_handle(2,0,0)
                elif event in ('heartbeat','sleep'):
                    C.c_uint32.in_dll(c,'sys_healths').value=1;c.os_handle(2,0,0)
                    if event=='sleep': C.c_uint64.in_dll(c,'sys_wake_us').value=2**63
                    c.advance(2_000_000)
                    if event=='sleep': c.os_asleep()
                    else: c.os_handle(2,0,0)
                else: c.os_handle({'undef':3,'data':5,'prefetch':4,'unknown':99}[event],7,IMAGE)
                self.assertFalse(c.os_running());self.assertEqual(c.probe_epoch(),0)
                self.assertEqual(c.probe_phase(),3);self.assertEqual(c.us_used(),used)
                self.assertEqual(c.sys_open_handles(),0)
                self.assertEqual(bytes(Frame.in_dll(c,'os_frame')),bytes(C.sizeof(Frame)))
                self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
                c.os_request_stop();self.drain()
                self.assertEqual(c.probe_last(),last);self.assertEqual(c.us_used(),0)

    def test_full_pool_quanta_repeated_stop_and_recovery_service(self):
        c=self.c;c.us_pool.restype=C.c_void_p
        self.assertEqual(c.us_reset(c.kernel_table()),0)
        self.assertEqual(c.us_map(IMAGE,16<<20,6),0)
        p=c.us_pool();C.memset(p,0x5a,16<<20)
        c.os_request_stop();c.os_task();last=c.probe_last()
        self.assertEqual(c.probe_cursor(),4096)
        for i in range(1,4096):
            c.os_request_stop();c.os_task()
            self.assertEqual(c.probe_cursor(),(i+1)*4096)
            self.assertEqual(c.probe_last(),last)
            if i<4095:
                self.assertEqual(c.us_used(),16<<20)
                self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
        self.assertEqual(c.us_used(),0);self.assertFalse(c.os_stop_pending())
        self.assertEqual(c.os_stop_result(),1)
        self.assertEqual(C.string_at(p,16<<20),bytes(16<<20))
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)

    def test_corrupt_owned_l1_is_quarantined_without_reuse(self):
        c=self.c;table=(C.c_uint32*4096).in_dll(c,'us_table')
        table[IMAGE>>20]^=0x400
        used=c.us_used();c.os_handle(5,0,0)
        self.assertEqual(c.probe_epoch(),0);self.assertFalse(c.os_running())
        for _ in range(3):
            c.os_task();self.assertEqual(c.us_used(),used)
            self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
        c.os_request_stop();c.os_task()
        self.assertEqual(c.os_stop_result(),-22)
        self.assertEqual(c.us_used(),used);self.assertEqual(c.probe_cursor(),0)

    def test_nested_launch_during_real_sd_read_is_busy(self):
        self.finish();c=self.c
        c.probe_run_on_read()
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
        self.assertEqual(c.probe_last(),2)
        self.assertEqual(c.probe_run_result(),-16)

    def test_clean_return_quarantines_and_scrubs_before_reuse(self):
        c=self.c
        c.us_pool.restype=C.c_void_p
        used=c.us_used(); pool=c.us_pool()
        C.memset(pool,0x91,used)
        f=Frame.in_dll(c,'os_frame');f.pc=0xf000;f.r[0]=0
        c.os_handle(4,0,0xf000)
        self.assertFalse(c.os_running())
        self.assertEqual(bytes(Frame.in_dll(c,'os_frame')),bytes(C.sizeof(Frame)))
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
        c.os_task()
        self.assertEqual(C.string_at(pool,4096),bytes(4096))
        self.assertEqual(C.string_at(pool+4096,1),b'\x91')
        self.assertEqual(c.us_used(),used)
        for _ in range(used//4096): c.os_task()
        self.assertEqual(c.us_used(),0)
        self.assertEqual(C.string_at(pool,used),bytes(used))
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)

    def test_fault_restart_waits_for_scrub_and_stop_cancels_intent(self):
        c=self.c;starts=C.c_uint32.in_dll(c,'os_starts').value
        used=c.us_used();c.os_handle(5,1,IMAGE)
        self.assertFalse(c.os_running())
        self.assertEqual(C.c_uint32.in_dll(c,'os_starts').value,starts)
        self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
        c.os_task()
        self.assertEqual(c.us_used(),used)
        c.os_request_stop()
        for _ in range(used//4096+2): c.os_task()
        self.assertFalse(c.os_running())
        self.assertEqual(c.us_used(),0)
        self.assertEqual(C.c_uint32.in_dll(c,'os_starts').value,starts)

    def test_fault_restart_and_second_fault_reclaim(self):
        c=self.c;used=c.us_used();c.os_handle(5,1,IMAGE)
        self.assertFalse(c.os_running())
        for _ in range(used//4096): c.os_task()
        self.assertTrue(c.os_running())
        c.os_handle(3,0,0)
        self.assertFalse(c.os_running())
        for _ in range(used//4096): c.os_task()
        self.assertFalse(c.os_running());self.assertEqual(c.us_used(),0)

if __name__=='__main__': __import__('unittest').main()
