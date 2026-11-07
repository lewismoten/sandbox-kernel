# SPDX-License-Identifier: MIT
"""M1 slice 4 host tests: clock_gettime (13) and sleep_until (14), ABI 1.1.
The clock is the kernel's 64-bit microsecond clock in nanoseconds (the input
event clock); a sleeping program is not entered until its deadline, and the
heartbeat rules keep running while it waits. Real core code (tests/harness.c)."""
import ctypes as C
import struct, unittest
import test_m1_user as u
from test_m1_user import IMAGE, peek
EINVAL,EFAULT=22,14

class Clock(u.SupervisorBase):
    def clock(self,sel=0,ns=0,size=32,flags=0):
        r=self.call(13,self.hdr(size,flags)+struct.pack('<2Q',sel,ns)[:size-16])
        return r,struct.unpack_from('<Q',peek(self.c,self.REC,32),24)[0]
    def sleep(self,deadline,size=24):
        return self.svc(IMAGE+4,14,self.hdr(size)+struct.pack('<Q',deadline)[:size-16])
    def health(self,seq): return self.svc(IMAGE+4,17,self.hdr(32)+struct.pack('<2Q',1,seq))
    def wake(self): return C.c_uint64.in_dll(self.c,'sys_wake_us').value
    def test_monotonic_ns(self):
        r,a=self.clock();self.assertEqual(r,0)
        self.c.advance(54_925);r,b=self.clock();self.assertEqual(r,0);self.assertEqual(b-a,54_925_000)
        self.assertEqual(b%1000,0)                                               # microsecond steps
    def test_clock_refusals(self):
        self.assertEqual(self.clock(sel=1)[0],-EINVAL)                           # only the monotonic clock
        self.assertEqual(self.clock(ns=5)[0],-EINVAL)                            # output must be zero on entry
        self.assertEqual(self.clock(size=24)[0],-EINVAL)
        self.assertEqual(self.clock(flags=1)[0],-EINVAL)
    def test_sleep_until(self):
        self.health(1);r,now=self.clock()
        self.assertEqual(self.sleep(now+30_000_000),0);self.assertEqual(self.wake()*1000,now+30_000_000)
        self.assertTrue(self.c.os_asleep())
        self.c.advance(29_999);self.assertTrue(self.c.os_asleep())
        self.c.advance(1);self.assertFalse(self.c.os_asleep());self.assertEqual(self.wake(),0)
        self.assertFalse(self.c.os_asleep())                                     # awake stays awake
    def test_deadline_rounds_up(self):
        self.health(1);r,now=self.clock()
        self.sleep(now+1);self.assertEqual(self.wake()*1000,now+1000)            # never wakes early
    def test_past_deadline_returns_at_once(self):
        self.health(1);self.c.advance(5000);r,now=self.clock()
        for d in (0,now-1000,now):
            with self.subTest(d): self.assertEqual(self.sleep(d),0);self.assertFalse(self.c.os_asleep())
    def test_sleep_refusals(self):
        self.assertEqual(self.sleep(1,size=16),-EINVAL)
        self.assertEqual(self.svc(IMAGE+4,14,self.hdr(32)+struct.pack('<2Q',1,1)),-EINVAL)   # unknown nonzero tail
    def test_heartbeat_runs_while_asleep(self):
        self.health(1);self.c.take_log();r,now=self.clock()
        self.sleep(now+5_000_000_000)
        for _ in range(199): self.c.advance(10_000);self.assertTrue(self.c.os_asleep())
        self.c.advance(10_000);self.assertTrue(self.c.os_asleep())
        self.assertIn(b'OS fault heartbeat missed 2 s',self.c.take_log())
        self.assertEqual(self.wake(),0)                                          # the restart clears the sleep
    def test_stop_clears_the_sleep(self):
        self.health(1);r,now=self.clock();self.sleep(now+1_000_000)
        self.c.sys_reset();self.assertEqual(self.wake(),0)

if __name__=='__main__': unittest.main()
