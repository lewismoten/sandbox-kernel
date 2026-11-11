# SPDX-License-Identifier: MIT
"""STOP exercises real supervisor/protocol code; no ARM execution on host."""
import ctypes as C
import os, pty, select, subprocess, sys, time, tty
from test_m1_user import ProtocolBase, CallsBase, program, Frame, IMAGE, peek, ROOT


class Stop(ProtocolBase):
    REC=CallsBase.REC;BUF=CallsBase.BUF
    call=CallsBase.call;open=CallsBase.open;io=CallsBase.io;mmap=CallsBase.mmap
    hdr=staticmethod(CallsBase.hdr)
    def command(self, text):
        data=(text+'\n').encode()
        self.c.feed(data,len(data));self.c.protocol_task()
        return self.c.take_output().decode()

    def boundary(self):
        self.c.os_task()
        for _ in range(4097):
            if not self.c.os_stop_pending(): break
            self.c.os_task()
        self.assertFalse(self.c.os_stop_pending())
        self.c.protocol_task()
        return self.c.take_output().decode()

    def test_pending_slot_reports_busy_and_refuses_put2(self):
        control=(C.c_uint32*128).in_dll(self.c,'control');old=control[4]
        try:
            control[4]=0
            info=self.command('INFO');self.assertIn(' updates=busy ',info);self.assertIn(' features=kernel,no-os',info)
            self.assertEqual(self.command('PUT2 kernel zero-w-armv6 64 00000000'),'ERR not-ready\n')
        finally: control[4]=old
        self.assertIn(' updates=ready ',self.command('INFO'))

    def test_active_stop_waits_for_safe_boundary(self):
        self.assertIn('INFO 2 ',self.command('INFO'))
        self.assertIn('OK file',self.put(program()))
        self.c.fb_setup(0,1)
        self.assertIn('OK run',self.command('RUN DIAG.ELF'))
        self.c.input_init()
        self.assertEqual(self.open('/dev/input/event0',0x100)[0],0)
        r,fb=self.open('/dev/fb0',2);self.assertEqual(r,0)
        self.assertEqual(self.mmap(fb,640*480*4),(0,0x40000000))
        self.assertEqual(self.c.sys_open_handles(),2)
        self.assertEqual(self.command('KEY 1 down'),'OK key 1 down\n')
        C.c_uint64.in_dll(self.c,'sys_wake_us').value=99999999
        starts=C.c_uint32.in_dll(self.c,'os_starts').value
        self.assertEqual(self.command('STOP'),'')
        # Parsing may occur reentrantly in SD I/O: do not destroy live state yet.
        self.assertTrue(self.c.os_running())
        self.assertGreater(self.c.us_used(),0)
        self.assertEqual(self.boundary(),'OK stop\n')
        self.assertFalse(self.c.os_running())
        self.assertEqual(self.c.us_used(),0)
        self.assertEqual(C.c_uint.in_dll(self.c,'us_count').value,0)
        self.assertEqual(bytes(Frame.in_dll(self.c,'os_frame')),bytes(C.sizeof(Frame)))
        self.assertEqual(self.c.sys_open_handles(),0)
        self.assertEqual(C.c_uint64.in_dll(self.c,'sys_wake_us').value,0)
        self.assertEqual(C.c_uint32.in_dll(self.c,'os_starts').value,starts)
        self.assertEqual(self.c.input_open(self.c.input_instance()),0)
        keys=(C.c_uint64*12)();axes=(C.c_int64*32)()
        self.assertEqual(self.c.input_state(self.c.input_instance(),keys,axes),0)
        self.assertFalse(any(keys));self.assertFalse(any(axes))
        self.assertIn(b'OS stopped by USB; recovery console',self.c.take_log())

    def test_usblog_during_sd_read_waits_for_foreground(self):
        self.put(program());self.command('RUN DIAG.ELF')
        r,fd=self.open('/sd/MIT.TXT');self.assertEqual(r,0)
        self.c.feed(b'USBLOG 0\n',9);self.c.protocol_on_next_sd_read()
        r,n=self.io(2,fd,self.BUF,32);self.assertEqual((r,n),(0,32))
        self.assertEqual(self.c.take_output(),b'')  # parsed mid-read, but no FAT access yet
        self.c.protocol_card_task()
        out=self.c.take_output().decode()
        self.assertTrue(out.startswith('USBLOG 0/') or out=='ERR usblog-unreadable\n',out)

    def test_stop_during_read_does_not_revoke_inflight_syscall(self):
        self.put(program());self.command('RUN DIAG.ELF')
        r,fd=self.open('/sd/MIT.TXT');self.assertEqual(r,0)
        self.assertEqual(self.c.sys_open_handles(),1)
        self.c.feed(b'STOP\n',5);self.c.protocol_on_next_sd_read()
        # The harness pumps protocol inside real asset_read, as board SD does.
        r,n=self.io(2,fd,self.BUF,32)
        self.assertEqual((r,n),(0,32))
        self.assertEqual(peek(self.c,self.BUF,32),(self.folder/'MIT.TXT').read_bytes()[:32])
        self.assertTrue(self.c.os_running());self.assertEqual(self.c.sys_open_handles(),1)
        self.assertEqual(self.c.take_output(),b'')
        self.assertEqual(self.boundary(),'OK stop\n')
        self.assertEqual(self.c.sys_open_handles(),0)
        self.command('RUN DIAG.ELF')
        self.assertEqual(self.io(2,fd,self.BUF,1)[0],-9)

    def test_stop_cancels_autostart_and_preserves_absent_device(self):
        self.put(program())
        self.c.fb_setup(0,1)
        self.c.autostart_run.argtypes=[C.c_char_p]
        self.assertEqual(self.c.autostart_run(b'/OS/DIAG.ELF'),0)
        self.c.input_init();self.c.input_remove()
        self.command('STOP');self.assertEqual(self.boundary(),'OK stop\n')
        self.assertTrue(self.c.autostart_cancelled())
        self.assertFalse(self.c.autostart_fallback())
        self.assertFalse(self.c.input_present())
        self.assertEqual(tuple((C.c_uint32*3).in_dll(self.c,'fb0')),(960,540,32))

    def test_failed_cleanup_reports_error_and_cannot_resume_old_frame(self):
        self.put(program());self.command('RUN DIAG.ELF')
        k=self.c.kernel_table().contents;k[IMAGE>>20]=0x402
        self.command('STOP')
        self.assertEqual(self.boundary(),'ERR stop USER MAP CONFLICT\n')
        self.assertFalse(self.c.os_running())
        self.assertEqual(bytes(Frame.in_dll(self.c,'os_frame')),bytes(C.sizeof(Frame)))
        # RUN is rejected by lifecycle admission before attempting another load.
        # Keep the original cleanup diagnosis and policy intact during quarantine.
        before=C.c_uint32.in_dll(self.c,'os_policy_state').value
        self.assertEqual(self.command('RUN DIAG.ELF'),'ERR not-ready\n')
        self.assertEqual(C.c_uint32.in_dll(self.c,'os_policy_state').value,before)
        k[IMAGE>>20]=0
        self.command('STOP');self.assertEqual(self.boundary(),'OK stop idle\n')
        self.assertIn('OK run',self.command('RUN DIAG.ELF'))

    def test_idle_stop_is_idempotent_and_requires_session(self):
        self.assertEqual(self.command('STOP'),'ERR not-ready\n')
        self.command('INFO')
        for _ in range(2):
            self.assertEqual(self.command('STOP'),'')
            self.assertEqual(self.boundary(),'OK stop idle\n')
        self.assertEqual(C.c_uint32.in_dll(self.c,'os_stops').value,0)
        self.assertEqual(self.command('STOP extra'),'ERR command\n')

    def test_pending_stop_survives_usb_reset_and_blocks_queued_run(self):
        self.put(program());self.command('RUN DIAG.ELF')
        self.command('STOP');self.c.protocol_reset()
        data=b'INFO\nRUN DIAG.ELF\n'
        self.c.feed(data,len(data))
        for _ in range(3): self.c.protocol_task()
        self.assertEqual(self.c.take_output(),b'')
        self.assertEqual(C.c_uint32.in_dll(self.c,'os_starts').value,1)
        self.assertEqual(self.boundary(),'OK stop\n')
        self.c.protocol_task();self.assertIn(b'INFO 2 ',self.c.take_output())
        self.c.protocol_task();self.assertEqual(self.c.take_output(),b'OK run /OS/DIAG.ELF\n')

    def test_stop_bytes_inside_put_payload_are_not_a_command(self):
        self.assertIn('OK file',self.put(b'STOP\n','TEXT.TXT'))
        self.assertEqual(self.read('/OS/TEXT.TXT'),b'STOP\n')
        self.assertFalse(self.c.os_stop_pending())

    def test_real_serial_cli_stop_through_pty_and_core(self):
        self.put(program());self.command('RUN DIAG.ELF')
        master,slave=pty.openpty();tty.setraw(slave)
        process=subprocess.Popen([sys.executable,str(ROOT/'scripts/usb_kernel.py'),os.ttyname(slave),'stop'],
                                 stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True)
        try:
            end=time.monotonic()+15
            while process.poll() is None and time.monotonic()<end:
                if select.select([master],[],[],0.02)[0]:
                    data=os.read(master,1024);self.c.feed(data,len(data))
                self.c.protocol_task();self.c.os_task();self.c.protocol_task()
                data=self.c.take_output()
                if data: os.write(master,data)
            self.assertIsNotNone(process.poll(),'serial stop exceeded deadline')
            out,err=process.communicate(timeout=2)
            self.assertEqual(process.returncode,0,out+err)
            self.assertIn('INFO 2 ',out);self.assertIn('OK stop',out)
            self.assertFalse(self.c.os_running())
            self.assertIn('OK file',self.put(program(),'GMOS.ELF'))
            self.assertIn('OK run',self.command('RUN GMOS.ELF'))
        finally:
            if process.poll() is None: process.kill();process.wait()
            if process.stdout is not None: process.stdout.close()
            if process.stderr is not None: process.stderr.close()
            os.close(master);os.close(slave)

    def test_not_ready_refusals_leave_the_err_line_alone_but_real_errors_stick(self):
        err=lambda: C.c_char_p.in_dll(self.c,'last_error').value
        before=err()
        self.assertEqual(self.command('STOP'),'ERR not-ready\n')     # no INFO session
        self.assertEqual(self.command('RUN DIAG.ELF'),'ERR not-ready\n')
        self.assertEqual(self.command('INPUT add'),'ERR not-ready\n')
        self.assertEqual(err(),before)
        info=self.command('INFO');self.assertIn(' updates=',info);self.assertIn(' os=none ',info)
        self.assertIn(' features=kernel,no-os',info) # legacy token kept for old clients
        self.assertIn('OK file',self.put(program()))
        self.assertIn('OK run',self.command('RUN DIAG.ELF'))
        self.assertIn(' os=running ',self.command('INFO'))
        self.assertIn('ERR not-ready',self.put(b'x'))                # an OS runs: normal refusal
        self.assertEqual(err(),before)
        self.assertEqual(self.command('RUN'+' '),'ERR command\n')    # a real error is sticky
        self.assertEqual(err(),b'command')

    def test_repeated_stop_put_run_and_normal_not_ready(self):
        self.command('INFO')
        for _ in range(4):
            self.assertIn('OK file',self.put(program()))
            self.assertIn('OK run',self.command('RUN DIAG.ELF'))
            frame=bytes(Frame.in_dll(self.c,'os_frame'))
            self.assertEqual(self.command('RUN DIAG.ELF'),'ERR not-ready\n')
            self.assertIn('ERR not-ready',self.put(b'x'))
            self.assertEqual(bytes(Frame.in_dll(self.c,'os_frame')),frame)
            self.assertEqual(self.command('STOP'),'')
            self.assertEqual(self.boundary(),'OK stop\n')
            # A queued IRQ/old frame must not restart a stopped program.
            self.c.os_handle(2,0,0)
            self.assertFalse(self.c.os_running())
        self.assertEqual(C.c_uint32.in_dll(self.c,'os_starts').value,4)
        self.assertEqual(C.c_uint32.in_dll(self.c,'os_stops').value,4)
