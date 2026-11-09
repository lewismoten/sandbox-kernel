# SPDX-License-Identifier: MIT
"""ABI 1.6 through actual supervisor/loader/dispatcher; host execution only."""
import ctypes as C
import hashlib
import struct
import test_supervisor_reclaim as reclaim
from test_m1_user import CallsBase, Work, peek, poke, START_VA, IMAGE, Frame, R, W
import hostlib
BASE,CAP=0x30000000,2195456

class ArenaABI(CallsBase):
    @classmethod
    def setUpClass(cls):
        Work.setUpClass.__func__(cls)
        hostlib.build(cls.work/'core.dylib',supervisor_probe=True)
    def setUp(self):
        super().setUp()
        self.c.probe_epoch.restype=C.c_uint64
        self.c.probe_last.restype=C.c_uint64
    drain=reclaim.Reclaim.drain
    finish=reclaim.Reclaim.finish
    def arena(self,epoch=None,requested=1,tail=b'',address=0,actual=0):
        epoch=self.c.probe_epoch() if epoch is None else epoch
        return self.hdr(48+len(tail))+struct.pack('<4Q',epoch,requested,address,actual)+tail
    def snapshot(self):
        c=self.c;c.us_pool.restype=C.c_void_p
        # Physical unused bytes are scrubbed during rollback; only live prefix
        # belongs to another mapping and must be byte-identical.
        return (c.us_used(),c.probe_l2_count(),C.c_uint.in_dll(c,'us_count').value,
                bytes((C.c_uint32*4096).in_dll(c,'us_table')),
                bytes(((C.c_uint32*256)*16).in_dll(c,'us_l2')),
                bytes((C.c_uint32*32).in_dll(c,'us_regions')),
                hashlib.sha256(C.string_at(c.us_pool(),c.us_used())).digest())
    def rejected(self,rec,want,**kw):
        poke(self.c,self.REC,rec);before=self.snapshot()
        self.assertEqual(self.call(21,None,size=len(rec),**kw),want)
        self.assertEqual(peek(self.c,self.REC,len(rec)),rec)
        self.assertEqual(self.snapshot(),before)
    def test_acquire_fixed_charge_and_idempotence(self):
        before=self.c.us_used();tables=self.c.probe_l2_count()
        regions=C.c_uint.in_dll(self.c,'us_count').value
        self.c.us_pool.restype=C.c_void_p
        C.memset(self.c.us_pool()+before,0xc7,CAP) # dirty unexposed backing, not a live user mapping
        self.assertEqual(self.call(21,self.arena()),0)
        self.assertEqual(struct.unpack_from('<2Q',peek(self.c,self.REC,48),32),(BASE,CAP))
        self.assertEqual(self.c.us_used()-before,CAP)
        self.assertEqual(self.c.probe_l2_count()-tables,3)
        self.assertEqual(C.c_uint.in_dll(self.c,'us_count').value-regions,1)
        self.assertEqual(peek(self.c,BASE,CAP),bytes(CAP))
        poke(self.c,BASE,b'SENTINEL')
        for n in (1,4095,4096,CAP-1,CAP):
            self.assertEqual(self.call(21,self.arena(requested=n)),0)
            self.assertEqual(self.c.us_used()-before,CAP)
            self.assertEqual(peek(self.c,BASE,8),b'SENTINEL')
        self.assertEqual(self.c.us_check(BASE-4096,4096,R),-14)
        self.assertEqual(self.c.us_check(BASE+CAP,1,R),-14)
        l1=(C.c_uint32*4096).in_dll(self.c,'us_table')
        l2=((C.c_uint32*256)*16).in_dll(self.c,'us_l2')
        bypa={self.c.us_l2_pa(j):j for j in range(16)}
        for p in range(CAP//4096):
            va=BASE+p*4096;d=l2[bypa[l1[va>>20]&~1023]][va>>12&255]
            self.assertEqual(d&4095,0x3f)
            self.assertEqual(d&~4095,0x04000000+before+p*4096)
    def test_startup_extension(self):
        s=peek(self.c,START_VA,48)
        self.assertEqual(struct.unpack_from('<IHH',s),(48,1,8))
        self.assertEqual(struct.unpack_from('<Q',s,40)[0],self.c.probe_epoch())
        self.assertEqual(struct.unpack_from('<3Q',s,16),(8<<20,1<<20,16))
        self.assertEqual(self.c.us_check(START_VA,48,W),-14)
    def test_record_order_and_unchanged_rejections(self):
        for n in (0,CAP+1,2**32-1,2**64-1): self.rejected(self.arena(requested=n),-22)
        for epoch in (0,2,2**64-1): self.rejected(self.arena(epoch=epoch,requested=0),-9)
        for major in (0,2): self.rejected(self.arena(),-93,major=major)
        for i in range(4):
            e=[0]*4;e[i]=1;self.rejected(self.arena(),-22,extra=e)
        for off in (4,8,12,32,40):
            r=bytearray(self.arena());r[off]=1;self.rejected(bytes(r),-22)
        self.rejected(self.arena(tail=b'\1'+bytes(7)),-22)
        self.rejected(self.arena(tail=bytes(4096-49)+b'\1'),-22)
        for n in (0,15,16,40,47,4104):
            self.assertEqual(self.call(21,None,size=n),-22)
        for i in range(1,8): self.assertEqual(self.call(21,None,size=48,va=self.REC+i),-14)
        self.assertEqual(self.call(22,None,size=0,major=0),-38)
        self.c.os_request_stop()
        self.rejected(self.arena(epoch=0,requested=0),-16)
        self.rejected(self.arena(address=1),-22)
    def test_zero_tail_56_and_4096(self):
        for n in (56,4096):
            r=self.arena(tail=bytes(n-48));self.assertEqual(self.call(21,r),0)
            out=peek(self.c,self.REC,n)
            self.assertEqual(out[48:],bytes(n-48))
            self.assertEqual(struct.unpack_from('<2Q',out,32),(BASE,CAP))
    def test_full_record_permissions_and_gaps(self):
        c=self.c
        self.assertEqual(self.call(21,self.arena(),va=IMAGE),-14)
        for va in (0,0x7ffffff8,0xfffffff8,self.BUF+4096-40):
            self.assertEqual(self.call(21,None,size=48,va=va),-14)
        # Readable prefix followed by a read-only suffix: whole-record W check.
        self.assertEqual(c.us_map(IMAGE+0x3000,4096,R),0)
        rec=self.arena();poke(c,self.BUF+4096-24,rec[:24]);poke(c,IMAGE+0x3000,rec[24:])
        before=self.snapshot()
        self.assertEqual(self.call(21,None,size=48,va=self.BUF+4096-24),-14)
        self.assertEqual(self.snapshot(),before)
    def test_readonly_prevalidation_never_touches_unused_pool(self):
        c=self.c;c.us_pool.restype=C.c_void_p;used=c.us_used()
        C.memset(c.us_pool()+used,0x91,CAP)
        poke(c,IMAGE+256,self.arena()) # privileged test alias writes a valid read-only user record
        before=self.snapshot()
        self.assertEqual(self.call(21,None,size=48,va=IMAGE+256),-14)
        self.assertEqual(self.snapshot(),before)
        self.assertEqual(C.string_at(c.us_pool()+used,CAP),bytes([0x91])*CAP)
    def test_late_copy_failure_rolls_back_then_reuses(self):
        c=self.c;r=self.arena();poke(c,self.REC,r);before=self.snapshot()
        c.probe_late_copy_failure()
        self.assertEqual(self.call(21,None,size=48),-14)
        self.assertEqual(peek(c,self.REC,48),r);self.assertEqual(self.snapshot(),before)
        c.us_pool.restype=C.c_void_p
        self.assertEqual(C.string_at(c.us_pool()+c.us_used(),CAP),bytes(CAP))
        self.assertEqual(self.call(21,r),0)
    def test_corrupt_frontier_never_returns_to_user(self):
        c=self.c;r=self.arena();poke(c,self.REC,r)
        c.probe_corrupt_after_prepare()
        f=Frame.in_dll(c,'os_frame');f.r[0]=self.REC;f.r[1]=48;f.r[2]=1;f.r[7]=21;f.pc=IMAGE+4
        c.os_handle(1,0,0)
        self.assertFalse(c.os_running())
        self.assertEqual(c.probe_epoch(),0)
        self.assertEqual(c.probe_phase(),3)
        self.assertEqual(peek(c,self.REC,48),r)
        self.assertEqual(bytes(Frame.in_dll(c,'os_frame')),bytes(C.sizeof(Frame)))
        used=c.us_used();self.assertGreater(used,CAP)
        for _ in range(3):
            c.os_task();self.assertEqual(c.us_used(),used)
            self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-16)
    def test_stop_after_prepare_rolls_back(self):
        c=self.c;r=self.arena();poke(c,self.REC,r);before=self.snapshot()
        c.probe_stop_after_prepare()
        self.assertEqual(self.call(21,None,size=48),-16)
        self.assertEqual(peek(c,self.REC,48),r);self.assertEqual(self.snapshot(),before)
        self.assertEqual(c.probe_epoch(),1)
        c.os_task();self.drain();self.assertEqual(c.us_used(),0)
    def test_arena_first_framebuffer_first_and_floor(self):
        for first in (False,True):
            if first:
                self.finish();self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'),0)
            e,fd=self.open('/dev/fb0');self.assertEqual(e,0)
            if first: self.assertEqual(self.mmap(fd,1)[0],0)
            self.assertEqual(self.call(21,self.arena()),0)
            self.assertEqual(self.mmap(fd,1)[0],0)
            self.assertGreaterEqual((16<<20)-self.c.us_used(),3<<20)
    def test_each_resource_rejection_preserves_live_maps(self):
        c=self.c
        # Independent internal pressure, not proof a public app may add maps.
        for kind in ('regions','tables','floor'):
            if kind!='regions':
                self.finish();self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
            if kind=='regions':
                for i in range(3): self.assertEqual(c.us_map(IMAGE+0x100000+i*4096,4096,R|W),0)
            elif kind=='tables':
                self.assertEqual(c.us_map(IMAGE+0x100000,8<<20,R|W),0)
            else:
                # Internal pool-floor oracle: remove mandatory fixture maps so
                # table/region limits have slack. This is NOT ELF admission.
                self.assertEqual(c.us_reset(c.kernel_table()),0)
                boundary=(16<<20)-CAP-1228800-(3<<20)
                self.assertEqual(c.us_map(IMAGE,boundary+4096,R|W),0)
                self.assertLessEqual(c.probe_l2_count()+5,16)
            for _ in range(8): self.rejected(self.arena(),-12)
    def test_stale_epoch_reload_scrub_and_terminal_routes(self):
        c=self.c
        for event in ('return','fault','exit','stop'):
            epoch=c.probe_epoch();self.assertEqual(self.call(21,self.arena()),0)
            poke(c,BASE,b'PRIVATE!');used=c.us_used()
            if event=='return':
                f=Frame.in_dll(c,'os_frame');f.pc=0xf000;f.r[0]=0;c.os_handle(4,0,0)
            elif event=='fault': c.os_handle(5,1,IMAGE);c.os_request_stop()
            elif event=='exit':
                self.assertEqual(self.call(16,self.hdr(24)+struct.pack('<q',0)),1)
                # sys_call returns SYS_EXITED to actual supervisor via svc.
                f=Frame.in_dll(c,'os_frame');f.pc=IMAGE+4;f.r[0]=self.REC;f.r[1]=24;f.r[2]=1;f.r[7]=16;c.os_handle(1,0,0)
            else: c.os_request_stop();c.os_task()
            self.assertFalse(c.os_running());self.assertEqual(c.probe_epoch(),0)
            self.assertEqual(c.us_check(BASE,1,R),0) # software regions until scrub; no user entry
            self.drain();self.assertEqual(c.us_used(),0)
            c.us_pool.restype=C.c_void_p
            self.assertEqual(C.string_at(c.us_pool(),used),bytes(used))
            self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
            self.rejected(self.arena(epoch=epoch),-9)
            self.assertEqual(self.call(21,self.arena()),0)
            self.assertEqual(peek(c,BASE,CAP),bytes(CAP))
            self.finish();self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
    def test_every_terminal_path_with_live_arena(self):
        c=self.c
        events=('return0','return7','exit0','exit7','undef','data','prefetch','unknown','thumb','svc-fetch','ready','heartbeat','sleep')
        for event in events:
            epoch=c.probe_epoch();self.assertEqual(self.call(21,self.arena()),0)
            poke(c,BASE,b'PRIVATE!');used=c.us_used();f=Frame.in_dll(c,'os_frame')
            if event.startswith('return'):
                f.pc=0xf000;f.r[0]=int(event[6:]);c.os_handle(4,0,0)
            elif event.startswith('exit'):
                poke(c,self.REC,self.hdr(24)+struct.pack('<q',int(event[4:])))
                f.r[0]=self.REC;f.r[1]=24;f.r[2]=1;f.r[7]=16;f.pc=IMAGE+4;c.os_handle(1,0,0)
            elif event=='thumb': f.cpsr=0x30;c.os_handle(1,0,0)
            elif event=='svc-fetch': f.pc=4;c.os_handle(1,0,0)
            elif event=='ready': c.advance(10000000);c.os_handle(2,0,0)
            elif event in ('heartbeat','sleep'):
                C.c_uint32.in_dll(c,'sys_healths').value=1;c.os_handle(2,0,0)
                if event=='sleep': C.c_uint64.in_dll(c,'sys_wake_us').value=2**63
                c.advance(2000000)
                if event=='sleep': c.os_asleep()
                else: c.os_handle(2,0,0)
            else: c.os_handle({'undef':3,'data':5,'prefetch':4,'unknown':99}[event],7,IMAGE)
            self.assertFalse(c.os_running());self.assertEqual(c.probe_epoch(),0)
            self.assertEqual(c.probe_phase(),3);self.assertEqual(c.us_used(),used)
            c.os_request_stop();self.drain();self.assertEqual(c.us_used(),0)
            c.us_pool.restype=C.c_void_p
            self.assertEqual(C.string_at(c.us_pool(),used),bytes(used))
            self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),0)
            self.rejected(self.arena(epoch=epoch),-9)
    def test_exact_internal_floor_idempotence(self):
        c=self.c
        self.assertEqual(c.us_reset(c.kernel_table()),0)
        boundary=(16<<20)-CAP-1228800-(3<<20)
        self.assertEqual(c.us_map(IMAGE,boundary,R|W),0)
        self.assertEqual(self.call(21,self.arena()),0)
        self.assertEqual(c.us_map(0x40000000,1228800,R|W),0)
        self.assertEqual((16<<20)-c.us_used(),3<<20)
        self.assertEqual(self.call(21,self.arena()),0)
    def test_reserved_kernel_sections_reject_launch(self):
        c=self.c;self.finish();k=c.kernel_table().contents
        for section in range(BASE>>20,(BASE+CAP-1>>20)+1):
            k[section]=0x123402
            self.assertEqual(c.os_start(b'/OS/DIAG.ELF'),-22)
            k[section]=0;self.drain()
            self.assertEqual(c.us_used(),0)

if __name__=='__main__': __import__('unittest').main()
