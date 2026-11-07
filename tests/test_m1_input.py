# SPDX-License-Identifier: MIT
"""M1 slice 3 host tests: the input event queue behind /dev/input/event0
(acceptance test 8; the only source is synthetic, labeled so), directory
listing (getdents), the health heartbeat in place of the 2-second stall rule
(test 5), the USB KEY/AXIS/INPUT commands, and PUTF into /GAMES/<DIR>.
Real core code over fake media (tests/harness.c)."""
import ctypes as C
import hashlib, os, struct, sys, unittest, zlib
import test_m1_user as u
from test_m1_user import IMAGE, poke, peek, text, fatgen, fsck
EAGAIN,EBUSY,ENODEV,EINVAL,EACCES,ENOTDIR,EFAULT=11,16,19,22,13,20,14
EV_SYN,EV_KEY,EV_ABS,SYN_REPORT,SYN_DROPPED=0,1,3,0,3
OPEN_NB=0x100

class Change(C.Structure): _fields_=[('type',C.c_uint16),('code',C.c_uint16),('value',C.c_int32)]

def frame(c,*changes):
    arr=(Change*max(1,len(changes)))(*[Change(*x) for x in changes]);return c.input_frame(arr,len(changes))

def records(data): return [struct.unpack_from('<QHHi',data,i) for i in range(0,len(data),16)]

class Input(u.CallsBase):
    def setUp(self):
        super().setUp();self.c.input_init()
    def ev(self):
        r,fd=self.open('/dev/input/event0',OPEN_NB);self.assertEqual(r,0);return fd
    def read(self,fd,count=1024):
        r,n=self.io(2,fd,self.BUF,count)
        return (r,records(peek(self.c,self.BUF,n)) if r==0 else [])
    def state(self,fd):
        q=self.BUF+0x400;poke(self.c,q,self.hdr(368)+bytes(352));r=self.ioctl(fd,0x30001,q,368)
        d=peek(self.c,q,368);return r,struct.unpack_from('<12Q',d,16),struct.unpack_from('<32q',d,112)
    def test_open_rules(self):
        self.assertEqual(self.open('/dev/input/event0',0)[0],-EINVAL)            # blocking reads need poll (later)
        self.assertEqual(self.open('/dev/input/event0',1|OPEN_NB)[0],-EACCES)    # read-only
        self.assertEqual(self.open('/dev/input/event1',OPEN_NB)[0],-ENODEV)
        fd=self.ev();self.assertEqual(self.open('/dev/input/event0',OPEN_NB)[0],-EBUSY)   # one reader at a time
        self.assertEqual(self.io(3,fd,self.BUF,16)[0],-9)                         # not writable
        self.assertEqual(self.close(fd),0);self.ev()
    def test_report_boundaries_and_timestamps(self):
        fd=self.ev();self.c.advance(1234)
        self.assertEqual(self.read(fd)[0],-EAGAIN)                                # empty, nonblocking
        self.assertEqual(frame(self.c,(EV_KEY,103,1)),0)
        r,ev=self.read(fd);self.assertEqual(r,0)
        t0=ev[0][0];self.assertEqual(ev,[(t0,EV_KEY,103,1),(t0,EV_SYN,SYN_REPORT,0)])
        self.c.advance(1500);frame(self.c,(EV_KEY,28,1),(EV_ABS,0,100),(EV_ABS,1,-5))
        got=[]
        while (x:=self.read(fd,16))[0]==0: self.assertEqual(len(x[1]),1);got+=x[1]   # one whole record per read
        self.assertEqual([g[1:] for g in got],[(EV_KEY,28,1),(EV_ABS,0,100),(EV_ABS,1,-5),(EV_SYN,SYN_REPORT,0)])
        self.assertEqual({g[0] for g in got},{t0+1_500_000})                     # one time per frame, in ns
        frame(self.c,(EV_KEY,28,0))
        self.assertEqual(self.read(fd,15)[0],-EINVAL)                             # too small for one record
        r,ev=self.read(fd,40);self.assertEqual(len(ev),2)                         # whole records only
        for buf in (IMAGE,0x00100000):
            frame(self.c,(EV_KEY,1,1));self.assertEqual(self.io(2,fd,buf,16)[0],-EFAULT)
    def test_only_changes_are_reported(self):
        fd=self.ev();frame(self.c,(EV_KEY,103,1));self.read(fd)
        for ch in ((EV_KEY,103,1),(EV_KEY,108,0),(EV_KEY,108,2),(EV_ABS,0,0)):    # no change: no events, no SYN
            with self.subTest(ch): self.assertEqual(frame(self.c,ch),0);self.assertEqual(self.read(fd)[0],-EAGAIN)
        frame(self.c,(EV_KEY,103,2));self.assertEqual([e[1:] for e in self.read(fd)[1]],[(EV_KEY,103,2),(EV_SYN,0,0)])   # repeat of a held key
        frame(self.c,(EV_KEY,57,1),(EV_KEY,57,0));self.assertEqual([e[1:] for e in self.read(fd)[1]],[(EV_KEY,57,1),(EV_KEY,57,0),(EV_SYN,0,0)])
    def test_bad_frames_change_nothing(self):
        fd=self.ev()
        for chs in ([(EV_KEY,0,1)],[(EV_KEY,768,1)],[(EV_KEY,1,3)],[(EV_KEY,1,-1)],[(EV_ABS,32,0)],[(EV_ABS,0,32768)],[(EV_ABS,0,-32769)],
                    [(2,0,1)],[(EV_SYN,0,0)],[(EV_KEY,1,1),(EV_KEY,999,1)],[],[(EV_KEY,1,1)]*17):
            with self.subTest(chs): self.assertEqual(frame(self.c,*chs),-EINVAL)
        self.assertEqual(self.read(fd)[0],-EAGAIN);r,keys,axes=self.state(fd);self.assertEqual((r,any(keys),any(axes)),(0,False,False))
        self.assertEqual(frame(self.c,(EV_ABS,31,-32768),(EV_ABS,31,32767)),0)
    def test_overflow_drops_then_resyncs(self):
        fd=self.ev()
        for i in range(40): frame(self.c,(EV_KEY,2+i,1))                        # 2 records each; 64 hold 32 frames
        r,ev=self.read(fd);self.assertEqual(r,0)
        self.assertEqual(ev[0][1:],(EV_SYN,SYN_DROPPED,0))                        # the queue was emptied: one SYN_DROPPED
        self.assertEqual([e[1:] for e in ev[1:]],[x for i in range(33,40) for x in ((EV_KEY,2+i,1),(EV_SYN,0,0))])
        self.assertEqual(C.c_uint32.in_dll(self.c,'input_dropped').value,1)
        frame(self.c,(EV_KEY,2,0))
        r,keys,axes=self.state(fd);self.assertEqual(r,0)                          # current state, then an empty queue
        down={w*64+b for w in range(12) for b in range(64) if keys[w]>>b&1};self.assertEqual(down,set(range(3,42)))
        self.assertEqual(self.read(fd)[0],-EAGAIN)
        frame(self.c,(EV_KEY,3,0));self.assertEqual([e[1:] for e in self.read(fd)[1]],[(EV_KEY,3,0),(EV_SYN,0,0)])
    def test_state_before_open_and_query(self):
        frame(self.c,(EV_KEY,28,1),(EV_ABS,1,-7))                                 # no reader: state only
        fd=self.ev();self.assertEqual(self.read(fd)[0],-EAGAIN)
        r,keys,axes=self.state(fd);self.assertEqual((r,keys[0]>>28&1,axes[1]),(0,1,-7))
        q=self.BUF+0x400;poke(self.c,q,self.hdr(72)+bytes(56));self.assertEqual(self.ioctl(fd,0x30000,q,72),0)
        self.assertEqual(struct.unpack_from('<5Q2q',peek(self.c,q,72),16),(1,1,64,768,32,-32768,32767))   # flag 1: synthetic
        poke(self.c,q,self.hdr(72)+struct.pack('<Q',1)+bytes(48));self.assertEqual(self.ioctl(fd,0x30000,q,72),-EINVAL)  # output not zero
        poke(self.c,q,self.hdr(64)+bytes(48));self.assertEqual(self.ioctl(fd,0x30000,q,64),-EINVAL)        # wrong size
        self.assertEqual(self.ioctl(fd,0x30002,q,72),-25)                          # unknown request
        r,log=self.open('/dev/log',1);self.assertEqual(self.ioctl(log,0x30000,q,72),-25)
    def test_removal(self):
        fd=self.ev();frame(self.c,(EV_KEY,28,1));self.c.input_remove()
        self.assertEqual(self.read(fd)[0],-ENODEV);self.assertEqual(self.state(fd)[0],-ENODEV)
        self.assertEqual(frame(self.c,(EV_KEY,28,0)),-ENODEV);self.assertEqual(self.open('/dev/input/event0',OPEN_NB)[0],-ENODEV)
        self.c.input_add();self.assertEqual(self.read(fd)[0],-ENODEV)             # an old handle never reads the new instance
        fd2=self.ev();r,keys,axes=self.state(fd2);self.assertEqual((r,any(keys)),(0,False))
        self.assertEqual(self.close(fd),0);self.assertEqual(self.open('/dev/input/event0',OPEN_NB)[0],-EBUSY)   # closing the old one leaves the new
        q=self.BUF+0x400;poke(self.c,q,self.hdr(72)+bytes(56));self.ioctl(fd2,0x30000,q,72)
        self.assertEqual(struct.unpack_from('<Q',peek(self.c,q,72),24)[0],2)
    def test_program_end_releases_reader(self):
        self.ev();self.c.sys_reset();self.ev()

class Dirents(u.CallsBase):
    def card(self,extra=None,order=1,fb=True):
        games={'DICE':{'DICE.GAM':b'GM'+bytes(300),'DICE.PAL':bytes(768)},'KITSHOP':{'KITSHOP.GAM':b'G'*5},'EMPTY':{},
               **{f'G{i:02d}':{'X.GAM':b'x'} for i in range(30)}}
        self.disk=self.work/f'games{id(self)}.img'
        fatgen.build(self.disk,{'MIT.TXT':b'mit','OS':{'DIAG.ELF':u.program()},'GAMES':games,'FILE.TXT':b'abc'})
        self.c.open_disk(os.fsencode(self.disk));self.assertEqual(self.c.asset_mount(),0);self.hw=self.c.fb_setup(order,fb)
        if self.c.os_start(b'/OS/DIAG.ELF'): self.fail(self.c.take_log())
    def entries(self,fd,size):
        out=[]
        while True:
            r,n=self.io(11,fd,self.BUF,size);self.assertEqual(r,0)
            if not n: return out
            d=peek(self.c,self.BUF,n);i=0
            while i<n:
                length,typ,nb,sz=struct.unpack_from('<HBBI',d,i);self.assertEqual(length%8,0)
                self.assertEqual(d[i+8+nb:i+length],bytes(length-8-nb));out.append((d[i+8:i+8+nb].decode(),typ,sz));i+=length
    def test_listing(self):
        r,fd=self.open('/sd/GAMES');self.assertEqual(r,0)
        big=self.entries(fd,4096);names=[e[0] for e in big]
        self.assertEqual(len(big),33);self.assertNotIn('.',names);self.assertNotIn('..',names)
        self.assertIn(('DICE',2,0),big);self.assertIn(('EMPTY',2,0),big)
        r,fd=self.open('/sd/GAMES/');self.assertEqual(self.entries(fd,24),big)    # one entry per call: nothing lost
        r,fd=self.open('/sd/GAMES/DICE');self.assertEqual(self.entries(fd,4096),[('DICE.GAM',1,302),('DICE.PAL',1,768)])
        r,fd=self.open('/sd/GAMES/EMPTY');self.assertEqual(self.entries(fd,64),[])
        r,fd=self.open('/sd');self.assertEqual({e[0] for e in self.entries(fd,4096)},{'MIT.TXT','OS','GAMES','FILE.TXT'})
    def test_small_buffer_keeps_the_entry(self):
        r,fd=self.open('/sd/GAMES/DICE')
        self.assertEqual(self.io(11,fd,self.BUF,8)[0],-EINVAL)                   # too small for DICE.GAM
        self.assertEqual(self.entries(fd,4096),[('DICE.GAM',1,302),('DICE.PAL',1,768)])
    def test_refusals(self):
        r,f=self.open('/sd/FILE.TXT');self.assertEqual(self.io(11,f,self.BUF,64)[0],-ENOTDIR)
        r,log=self.open('/dev/log',1);self.assertEqual(self.io(11,log,self.BUF,64)[0],-ENOTDIR)
        r,fd=self.open('/sd/GAMES')
        self.assertEqual(self.io(11,fd,IMAGE,64)[0],-EFAULT);self.assertEqual(self.io(11,fd,self.BUF,(1<<20)+1)[0],-EINVAL)
        self.assertEqual(self.io(11,fd,self.BUF,64,done=1)[0],-EINVAL);self.assertEqual(self.io(11,0x777,self.BUF,64)[0],-9)
        self.assertEqual(self.io(2,fd,self.BUF,64)[0],-EINVAL)                   # read() is not for directories

class Heartbeat(u.SupervisorBase):
    def health(self,seq,gen=1,size=32,reserved=0):
        return self.svc(IMAGE+4,17,self.hdr(size,0,reserved)+struct.pack('<2Q',gen,seq)[:size-16])
    def irq(self): self.c.os_handle(2,0,0)
    def test_readiness_within_10_s(self):
        self.c.take_log();self.c.advance(9_990_000);self.irq();self.assertTrue(self.c.os_running())
        rec=self.hdr(56)+struct.pack('<5Q',self.BUF+0x800,8,1,0,0);poke(self.c,self.BUF+0x800,b'/dev/log')
        self.svc(IMAGE+4,1,rec);self.assertTrue(self.c.os_running())              # other calls are not a readiness report
        self.c.advance(10_000);log=self.c.take_log();self.svc(IMAGE+4,1,rec);log=self.c.take_log().decode()
        self.assertIn('OS fault no readiness report in 10 s',log);self.assertIn('OS restart',log)
    def test_ready_then_every_2_s(self):
        self.c.take_log();self.c.advance(1_000_000);self.assertEqual(self.health(1),0)
        self.assertIn(b'OS ready /OS/DIAG.ELF after 1000 ms',self.c.take_log())
        for seq in (2,3,4):
            self.c.advance(1_990_000);self.irq();self.assertTrue(self.c.os_running());self.assertEqual(self.health(seq),0)
        self.assertNotIn(b'OS ready',self.c.take_log())                          # logged once
        self.c.advance(1_990_000);self.irq();self.assertTrue(self.c.os_running())
        self.c.advance(10_000);log=self.stop_kinds(2,0,0,'heartbeat missed 2 s');self.assertIn('OS restart',log)
        self.assertEqual(self.health(1),0)                                       # a restart starts a new sequence
    def test_health_rules(self):
        for args,want in (((1,),0),((1,),-EINVAL),((0,),-EINVAL),((5,2),-EINVAL),((5,1,24),-EINVAL),((5,1,32,1),-EINVAL),((3,),0),((2,),-EINVAL)):
            with self.subTest(args): self.assertEqual(self.health(*args),want)
        self.assertEqual(self.health(2**64-1),0);self.assertEqual(self.health(2**64-1),-EINVAL)
    def test_refused_reports_do_not_count(self):
        self.c.take_log()
        for i in range(10): self.c.advance(1_000_000);self.health(0)
        self.assertIn(b'no readiness report in 10 s',self.c.take_log())
    def test_loop_program_preempted(self):
        self.health(1);self.c.take_log()
        for _ in range(199): self.c.advance(10_000);self.irq()                    # timer slices while it loops
        self.assertTrue(self.c.os_running());self.c.advance(10_000);self.irq()
        self.assertIn(b'OS fault heartbeat missed 2 s',self.c.take_log())

class Synthetic(u.ProtocolBase):
    def line(self,s):
        b=(s+'\n').encode();self.c.feed(b,len(b));return self.run_protocol(5).strip()
    def setUp(self):
        super().setUp();self.c.input_init()
    def test_key_axis_input(self):
        self.assertEqual(self.line('KEY 103 down'),'ERR not-ready')               # INFO first, as for PUTF
        self.line('INFO');self.c.take_output()
        self.assertEqual(self.c.input_open(self.c.input_instance()),0)
        for cmd,want in (('KEY 103 down','OK key 103 down'),('KEY 103 repeat','OK key 103 repeat'),('KEY 103 up','OK key 103 up'),
                         ('AXIS 0 -32768','OK axis 0 -32768'),('AXIS 1 32767','OK axis 1 32767'),
                         ('KEY 999 down','ERR input range'),('KEY 0 down','ERR input range'),('KEY 103 sideways','ERR input value'),
                         ('KEY x down','ERR input command'),('KEY 103','ERR input command'),('KEY 99999999999 up','ERR input command'),
                         ('AXIS 0 -32769','ERR input value'),('AXIS 0 32768','ERR input range'),('AXIS 40 1','ERR input range'),('AXIS 0 1x','ERR input value')):
            with self.subTest(cmd): self.assertEqual(self.line(cmd),want)
        buf=C.create_string_buffer(16*64);n=self.c.input_read(self.c.input_instance(),buf,64)
        self.assertEqual([r[1:] for r in records(buf.raw[:n*16])],[(EV_KEY,103,1),(EV_SYN,0,0),(EV_KEY,103,2),(EV_SYN,0,0),(EV_KEY,103,0),(EV_SYN,0,0),
                                                                  (EV_ABS,0,-32768),(EV_SYN,0,0),(EV_ABS,1,32767),(EV_SYN,0,0)])
        self.assertEqual(self.line('INPUT remove'),'OK input remove');self.assertEqual(self.line('KEY 28 down'),'ERR input no-device')
        self.assertEqual(self.line('INPUT add'),'OK input add');self.assertEqual(self.line('KEY 28 down'),'OK key 28 down')
        self.assertEqual(self.line('INPUT plug'),'ERR command')
    def put_path(self,path,data):
        return self.put(data,name=path)
    def test_putf_games(self):
        root=self.listing('/');free=self.free()
        gam=b'GM'+bytes(1313);pal=bytes(range(256))*3
        self.assertIn('OK file /GAMES/DICE/DICE.GAM 1315',self.put_path('GAMES/DICE/DICE.GAM',gam))
        self.assertIn('OK file /GAMES/DICE/DICE.PAL 768',self.put_path('GAMES/DICE/DICE.PAL',pal))
        self.assertIn('OK file /GAMES/KITSHOP/KITSHOP.GAM 5',self.put_path('GAMES/KITSHOP/KITSHOP.GAM',b'hello'))
        self.assertIn('OK file /OS/GMOS.ELF 3',self.put_path('GMOS.ELF',b'elf'))
        self.assertEqual(self.read('/GAMES/DICE/DICE.GAM'),gam);self.assertEqual(self.read('/GAMES/DICE/DICE.PAL'),pal)
        self.assertEqual(self.read('/GAMES/KITSHOP/KITSHOP.GAM'),b'hello')
        self.assertEqual(self.listing('/')-root,{'GAMES','OS'})                  # the only new root entries
        self.assertEqual(self.free(),free-4-(3+2+1+1))                             # GAMES, DICE, KITSHOP and OS, then 512-byte data clusters
        self.untouched()
        r=fsck(self.disk,self.work);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertNotIn('Fix',r.stdout,r.stdout)
    def listing(self,path):
        h=self.c.asset_open(path.encode(),len(path),0);out=set();d=(C.c_uint8*20)()
        while self.c.asset_readdir(h,d)>0: out.add(bytes(d[:13]).split(b'\0')[0].decode())
        self.c.asset_close(h);return out
    def test_putf_path_refusals(self):
        for path in ('GAMES/DICE.X/A.GAM','OTHER/DICE/A.GAM','GAMES/A.GAM','GAMES/A/B/C.GAM','games/dice/a.gam','GAMES//A.GAM',
                     'GAMES/DICE/','/GAMES/DICE/A.GAM','GAMES/TOOLONGDIR/A.GAM','GAMES/DICE/A.GAMS','OS/A.ELF','GAMES/../A.GAM'):
            with self.subTest(path): self.assertIn('ERR command',self.put_path(path,b'x'))
        self.untouched()
    def test_games_name_taken_by_a_file(self):
        self.c.close_disk();self.disk,self.folder=u.boot_card(self.work,{'GAMES':b'not a directory'})
        self.c.open_disk(os.fsencode(self.disk));self.c.asset_mount()
        self.assertEqual(self.c.fatput(b'GAMES/DICE/DICE.GAM',b'x',1),-ENOTDIR);self.assertEqual(text(self.c,'fatput_fail'),'PUT NOT DIR')

if __name__=='__main__': unittest.main()
