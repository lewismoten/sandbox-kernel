# SPDX-License-Identifier: MIT
"""Idle policy and real syscall/cleanup paths, using the normal host harness."""
import ctypes as C
import struct
import unittest
import test_m1_user as u
from test_m1_input import frame, EV_KEY

class Power(u.SupervisorBase):
    def setUp(self):
        super().setUp()
        self.c.clock_us.restype=C.c_uint64
        self.c.power_target.argtypes=[C.c_uint64]
        self.c.power_input.argtypes=[C.c_uint64]
        self.c.power_wait_us.argtypes=[C.c_uint64,C.c_uint64]
        self.c.power_reset.argtypes=[C.c_uint64]
        self.c.power_target.restype=C.c_uint32
        self.c.power_reset(self.c.clock_us())
    def hold(self, flags=3, token=0, op=1, tail=b''):
        r=self.call(22,self.hdr(40+len(tail))+struct.pack('<3Q',op,flags,token)+tail)
        return r,struct.unpack_from('<Q',u.peek(self.c,self.REC,40),32)[0]
    def asleep(self):
        r=self.hold(0,0,2)[0];return r,struct.unpack_from('<Q',u.peek(self.c,self.REC,40),24)[0]
    def release(self,t): return self.hold(0,t,0)[0]
    def timeouts(self,s,b):
        return self.call(23,self.hdr(32)+struct.pack('<2Q',s,b))
    def test_defaults_and_input_wake(self):
        now=self.c.clock_us()
        self.assertEqual(self.c.power_target(now+59999999),0)
        self.assertEqual(self.c.power_target(now+60000000),2)
        self.assertEqual(self.c.power_target(now+599999999),2)
        self.assertEqual(self.c.power_target(now+600000000),3)
        self.c.power_input(now+600000000)
        self.assertEqual(self.c.power_target(now+600000000),0)
        self.assertEqual(self.c.power_target(now+660000000),2)
    def test_holds_independent_and_release(self):
        now=self.c.clock_us()+7200000000
        a,ta=self.hold(1);b,tb=self.hold(2)
        self.assertEqual((a,b),(0,0));self.assertNotEqual(ta,tb)
        self.assertEqual(self.c.power_target(now),0)
        self.assertEqual(self.release(ta),0);self.assertEqual(self.c.power_target(now),1)
        self.assertEqual(self.release(ta),-9)
        self.assertEqual(self.release(tb),0);self.assertEqual(self.c.power_target(now),3)
    def test_timeout_disable_and_validation(self):
        now=self.c.clock_us()
        for s,b in [(86401,0),(0,86401),(2**32,0)]: self.assertEqual(self.timeouts(s,b),-22)
        self.assertEqual(self.timeouts(0,0),0)
        self.assertEqual(self.c.power_target(now+86400000000),0)
        self.assertEqual(self.timeouts(2,3),0)
        self.assertEqual(self.c.power_target(now+1999999),0)
        self.assertEqual(self.c.power_target(now+2000000),2)
        self.assertEqual(self.c.power_target(now+3000000),3)
    def test_hold_limits_and_bad_records(self):
        for flags in (0,4,2**32): self.assertEqual(self.hold(flags)[0],-22)
        self.assertEqual(self.hold(1,1)[0],-22)
        self.assertEqual(self.hold(1,0,2)[0],-22)
        self.assertEqual(self.release(0),-9)
        self.assertEqual(self.hold(tail=b'\1'+bytes(7))[0],-22)
        self.assertEqual(self.hold(tail=bytes(8))[0],0)
        for i in range(15): self.assertEqual(self.hold()[0],0)
        self.assertEqual(self.hold()[0],-24)
    def test_query_reports_what_is_asleep(self):
        self.assertEqual(self.timeouts(0,0),0)
        self.assertEqual(self.asleep(),(0,0))
        self.assertEqual(self.timeouts(10,20),0)
        now=self.c.clock_us()
        self.c.power_input(now-5000000);self.assertEqual(self.asleep(),(0,0))
        self.c.power_input(now-15000000);self.assertEqual(self.asleep(),(0,2))
        self.c.power_input(now-25000000);self.assertEqual(self.asleep(),(0,3))
        self.assertEqual(self.hold(1,0,2)[0],-22);self.assertEqual(self.hold(0,5,2)[0],-22)
        t=self.hold(1)[1];self.assertEqual(self.asleep(),(0,2));self.release(t)
        self.c.power_input(now);self.assertEqual(self.asleep(),(0,0))
    def test_cleanup_revokes_tokens_and_policy(self):
        _,t=self.hold();self.timeouts(0,0)
        self.c.sys_reset()
        self.assertEqual(self.release(t),-9)
        _,new=self.hold();self.assertNotEqual(t,new)
        self.release(new)
        self.assertEqual(self.c.power_target(self.c.clock_us()+600000000),3)
    def terminal_release(self,status):
        _,token=self.hold()
        self.svc(u.IMAGE+4,16,self.hdr(24)+struct.pack('<q',status))
        self.assertEqual(self.release(token),-9)
    def test_exit_release(self): self.terminal_release(0)
    def test_fault_release(self): self.terminal_release(1)
    def test_usb_stop_release(self):
        _,token=self.hold()
        self.c.os_request_stop();self.c.os_task()
        self.assertEqual(self.release(token),-9)
    def test_acquire_readonly_record_does_not_leak(self):
        record=self.hdr(40)+struct.pack('<3Q',1,3,0)
        u.poke(self.c,u.IMAGE,record)
        self.assertEqual(self.call(22,None,va=u.IMAGE,size=40),-14)
        for i in range(16): self.assertEqual(self.hold()[0],0)
    def test_input_without_reader_counts(self):
        self.c.input_init()
        n=C.c_uint32.in_dll(self.c,'input_activity').value
        frame(self.c,(EV_KEY,28,1))
        self.assertEqual(C.c_uint32.in_dll(self.c,'input_activity').value,n+1)
        frame(self.c,(EV_KEY,28,1))
        self.assertEqual(C.c_uint32.in_dll(self.c,'input_activity').value,n+1)
    def test_token_epoch_exhaustion(self):
        class Prefix(C.Structure):
            _fields_=[('last_input',C.c_uint64),('next_token',C.c_uint64)]
        Prefix.in_dll(self.c,'power0').next_token=2**64-1
        self.assertEqual(self.hold()[0],-75)
        self.c.sys_reset()
        self.assertEqual(self.hold()[0],-75)
    def test_idle_wakeup_model(self):
        # Same wait calculation as main: no interrupt traffic, 1 simulated second.
        now=0;wakeups=0
        while now<1000000:
            now+=self.c.power_wait_us(now,0);wakeups+=1
        self.assertEqual(wakeups,1000)
        self.assertEqual(self.c.power_wait_us(1000,1001),1)
        self.assertEqual(self.c.power_wait_us(1000,1000),0)
        self.assertEqual(self.c.power_wait_us(1000,999),0)
        self.assertEqual(self.c.power_wait_us(2**32,2**32+999),999)
        print('idle wake model: 1000 timer wakes/s; sleep deadline error 0 us')

if __name__=='__main__': unittest.main()
