# SPDX-License-Identifier: MIT
"""M1 slice 2 host tests (slice 3 adds tests/test_m1_input.py): the user address space and its page descriptors,
strict ELF32 checks with fuzzing (acceptance test 3), the system-call
dispatcher (acceptance test 4), the OS supervisor's fault and restart policy
(tests 2 and 5, first form), and the /OS file installer behind USB PUTF.
Real core code over fake media (tests/harness.c); User mode itself only runs
on the board."""
import ctypes as C
import hashlib, json, os, pathlib, random, shutil, struct, subprocess, sys, tempfile, unittest, zlib
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'));sys.path.insert(0,str(ROOT/'scripts'))
import fatgen, hostlib, image
EBADF,EFAULT,EINVAL,ENOENT,EROFS,ENOSYS,ENOTTY,EMFILE,EACCES,ENODEV,ENAMETOOLONG,EBUSY=9,14,22,2,30,38,25,24,13,19,36,16
IMAGE,IMAGE_END,FB_VA,START_VA,STACK_TOP,EXIT_VA=0x08000000,0x10000000,0x40000000,0x6ffff000,0x7fff0000,0x0000f000
POOL,POOL_BYTES=0x04000000,16<<20
R,W,X=4,2,1
SVC0,SVC1=0xef000000,0xef000001

class Seg(C.Structure): _fields_=[(n,C.c_uint32) for n in ('va','memsz','filesz','offset','prot')]
class Plan(C.Structure): _fields_=[('entry',C.c_uint32),('count',C.c_uint32),('bytes',C.c_uint32),('seg',Seg*4)]
class Frame(C.Structure): _fields_=[('r',C.c_uint32*13),('sp',C.c_uint32),('lr',C.c_uint32),('pc',C.c_uint32),('cpsr',C.c_uint32)]

def elf(segs,entry=None,ident=None,etype=2,machine=40,flags=0x05000200,extra=(),shdrs=(),phoff=52,version=1):
    """segs: (va, p_flags, data, memsz). extra: raw (type, flags) program headers."""
    ph=[];blobs=[];offset=4096
    for va,fl,data,memsz in segs:
        off=offset+(va%4096);ph.append(struct.pack('<8I',1,off,va,va,len(data),memsz,fl,4096));blobs.append((off,data))
        offset=(off+len(data)+4095)//4096*4096+4096
    for t,fl in extra: ph.append(struct.pack('<8I',t,0,0,0,0,0,fl,4))
    shoff=offset if shdrs else 0
    e=bytearray(ident or b'\x7fELF\x01\x01\x01'+bytes(9))
    e+=struct.pack('<HHIIIIIHHHHHH',etype,machine,version,IMAGE if entry is None else entry,phoff,shoff,flags,52,32,len(ph),40 if shdrs else 0,len(shdrs),0)
    out=bytearray(max(offset,phoff+32*len(ph))+40*len(shdrs));out[:52]=e
    for i,p in enumerate(ph): out[phoff+32*i:phoff+32*i+32]=p
    for off,data in blobs: out[off:off+len(data)]=data
    for i,(t,fl) in enumerate(shdrs): out[shoff+40*i:shoff+40*i+40]=struct.pack('<10I',0,t,fl,0,0,0,0,0,4,0)
    return bytes(out)

TEXT=struct.pack('<4I',SVC0,SVC1,0xe12fff1e,0)
def program(): return elf([(IMAGE,5,TEXT,16),(IMAGE+0x1000,6,b'D'*64,0x2000)])

def lib(work):
    out=work/'core.dylib'
    if not out.exists(): hostlib.build(out)
    d=pathlib.Path(tempfile.mkdtemp(dir=work));shutil.copyfile(out,d/'lib.dylib');c=C.CDLL(str(d/'lib.dylib'))
    c.elf_check.argtypes=[C.c_char_p,C.c_uint32,C.POINTER(Plan)];c.elf_check.restype=C.c_char_p
    c.elf_load.argtypes=[C.c_char_p,C.POINTER(Plan)]
    c.us_reset.argtypes=[C.c_void_p];c.us_map.argtypes=[C.c_uint32,C.c_uint32,C.c_uint32]
    c.us_span.argtypes=[C.c_uint32,C.c_uint32,C.c_uint32,C.POINTER(C.c_void_p)];c.us_span.restype=C.c_int64
    c.us_check.argtypes=[C.c_uint64,C.c_uint64,C.c_uint32];c.us_l2_pa.restype=C.c_uint32
    c.kernel_table.restype=C.POINTER(C.c_uint32*4096);c.take_log.restype=C.c_char_p;c.take_output.restype=C.c_char_p
    c.sys_call.argtypes=[C.POINTER(C.c_uint32*8)];c.os_start.argtypes=[C.c_char_p];c.os_running.restype=C.c_bool
    c.os_handle.argtypes=[C.c_uint,C.c_uint32,C.c_uint32];c.fb_setup.restype=C.c_void_p;c.fb_setup.argtypes=[C.c_uint32,C.c_int]
    c.fatput.argtypes=[C.c_char_p,C.c_char_p,C.c_uint32];c.open_disk.argtypes=[C.c_char_p]
    c.asset_open.argtypes=[C.c_char_p,C.c_uint32,C.c_uint32];c.asset_read.argtypes=[C.c_int,C.c_void_p,C.c_uint32]
    c.advance.argtypes=[C.c_uint32];c.feed.argtypes=[C.c_char_p,C.c_uint];c.initialize.restype=C.c_bool
    c.fb_pattern_pixel.argtypes=[C.c_void_p,C.c_uint32,C.c_uint32];c.fb_pattern_pixel.restype=C.c_uint32
    k=c.kernel_table().contents
    for i in range(128): k[i]=(i<<20)|0x402|(8 if i else 0)   # privileged identity RAM, as mmu_init
    for i in range(0x200,0x210): k[i]=(i<<20)|0x416          # MMIO, device, XN
    for i in range(0x1ea,0x1ec): k[i]=(i<<20)|0x1412         # framebuffer
    return c

def text(c,name): return C.c_char_p.in_dll(c,name).value.decode()
def drain(c):
    """Explicit main-loop passes; never replace production event dispatch."""
    for _ in range(4097):
        if not c.os_cleanup_pending(): return
        c.os_task()
    raise AssertionError('cleanup did not finish within the full-pool bound')
def span(c,va,n,need=R):
    p=C.c_void_p();got=c.us_span(va,n,need,C.byref(p));return got,p.value
def poke(c,va,data):
    got,p=span(c,va,len(data));assert got==len(data),(hex(va),got);C.memmove(p,data,len(data))
def peek(c,va,n):
    got,p=span(c,va,n);assert got==n;return C.string_at(p,n)

class Work(unittest.TestCase):
    @classmethod
    def setUpClass(cls): cls.temp=tempfile.TemporaryDirectory();cls.work=pathlib.Path(cls.temp.name)
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def setUp(self): self.c=lib(self.work)

class Elf(Work):
    def check(self,f):
        p=Plan();r=self.c.elf_check(f,len(f),C.byref(p));return (r.decode() if r else None),p
    def test_good_and_built_programs(self):
        r,p=self.check(program());self.assertIsNone(r);self.assertEqual((p.count,p.entry,p.bytes),(2,IMAGE,3*4096))
        self.assertEqual((p.seg[0].prot,p.seg[1].prot),(R|X,R|W))
        built=sorted((ROOT/'build/os').glob('*.ELF')) if (ROOT/'build/os').exists() else []
        for f in built:
            with self.subTest(f.name): self.assertIsNone(self.check(f.read_bytes())[0])
    def test_rejections(self):
        t=[(IMAGE,5,TEXT,16)]
        cases={
         'ELF64':elf(t,ident=b'\x7fELF\x02\x01\x01'+bytes(9)),'ELF ENDIAN':elf(t,ident=b'\x7fELF\x01\x02\x01'+bytes(9)),
         'ELF MAGIC':b'\x7fELG'+program()[4:],'ELF SHORT':program()[:51],'ELF PIE':elf(t,etype=3),'ELF TYPE':elf(t,etype=1),
         'ELF MACHINE':elf(t,machine=183),'ELF VERSION':elf(t,version=2),'ELF EABI':elf(t,flags=0x04000200),
         'ELF HARD FLOAT':elf(t,flags=0x05000400),'ELF FLAGS':elf(t,flags=0x05800000),'ELF THUMB':elf(t,entry=IMAGE|1),
         'ELF ENTRY ALIGN':elf(t,entry=IMAGE+2),'ELF ENTRY':elf(t,entry=IMAGE+16),
         'ELF INTERP':elf(t,extra=[(3,4)]),'ELF DYNAMIC':elf(t,extra=[(2,6)]),'ELF TLS':elf(t,extra=[(7,4)]),
         'ELF SHLIB':elf(t,extra=[(5,4)]),'ELF EXEC STACK':elf(t,extra=[(0x6474e551,7)]),'ELF PHDR TYPE':elf(t,extra=[(0x6474e552,4)]),
         'ELF WX':elf([(IMAGE,7,TEXT,16)]),'ELF SEG FLAGS':elf([(IMAGE,8|5,TEXT,16)]),
         'ELF OVERLAP':elf([(IMAGE,5,TEXT,16),(IMAGE+0x800,6,b'x',8)]),
         'ELF SEG RANGE':elf([(IMAGE,5,TEXT,16),(IMAGE_END-8,6,b'',16)]),
         'ELF QUOTA':elf([(IMAGE,5,TEXT,16),(IMAGE+0x1000,6,b'',9<<20)]),
         'ELF FILESZ':elf([(IMAGE,5,TEXT,8)]),'ELF NO LOAD':elf([],entry=IMAGE,extra=[(4,4)]),
         'ELF RELOC':elf(t,shdrs=[(0,0),(9,2)]),'ELF PHNUM':elf(t,extra=[(0,0)]*16),
         'ELF PHDR RANGE':elf(t)[:28]+struct.pack('<I',0x7ffffff0)+elf(t)[32:],
        }
        low=elf([(0x8000,5,TEXT,16)],entry=0x8000);cases['ELF SEG RANGE (null page)']=low
        kern=elf([(0x80000000,5,TEXT,16)],entry=0x80000000);cases['ELF SEG RANGE (kernel half)']=kern
        for want,f in cases.items():
            with self.subTest(want): self.assertEqual(self.check(f)[0],want.split(' (')[0])
        self.assertIsNone(self.check(elf(t,shdrs=[(0,0),(9,0)]))[0])   # a non-allocated REL section is ignored
        bad=bytearray(program());struct.pack_into('<I',bad,52+32+4,len(bad)-8);struct.pack_into('<I',bad,52+32+16,64)
        self.assertEqual(self.check(bytes(bad))[0],'ELF SEG FILE')
        mis=bytearray(program());struct.pack_into('<I',mis,52+32+4,0x2001)
        self.assertEqual(self.check(bytes(mis))[0],'ELF ALIGN')
    def invariant(self,f,p):
        pages=set();total=0
        for s in p.seg[:p.count]:
            self.assertTrue(IMAGE<=s.va and s.va+s.memsz<=IMAGE_END)
            self.assertLessEqual(s.filesz,s.memsz);self.assertLessEqual(s.offset+s.filesz,len(f))
            self.assertFalse(s.prot&W and s.prot&X)
            span=set(range(s.va>>12,(s.va+s.memsz+4095)>>12));self.assertFalse(span&pages);pages|=span
        self.assertLessEqual(len(pages)*4096,8<<20);self.assertEqual(p.bytes,len(pages)*4096)
        self.assertFalse(p.entry&3)
        self.assertTrue(any(s.prot&X and s.va<=p.entry and p.entry+4<=s.va+s.filesz for s in p.seg[:p.count]))
    def test_fuzz_mutations(self):
        rnd=random.Random(1107);base=program();accepted=0
        for i in range(6000):
            f=bytearray(base)
            for _ in range(rnd.randint(1,6)):
                at=rnd.randrange(0,52+32*2);f[at]=rnd.choice([0,1,0xff,0x7f,rnd.getrandbits(8)])
            if rnd.random()<0.3: struct.pack_into('<I',f,rnd.randrange(0,52+64-3)&~3,rnd.choice([0,0xffffffff,0x80000000,0x0fffffff,rnd.getrandbits(32)]))
            r,p=self.check(bytes(f))
            if r is None: accepted+=1;self.invariant(bytes(f),p)
        self.assertGreater(accepted,0)
    def test_fuzz_program_tables(self):
        rnd=random.Random(5);accepted=0
        vals=[0,1,0xfff,0x1000,IMAGE,IMAGE+0x1000,IMAGE_END-0x1000,IMAGE_END,0x7fffffff,0xfffff000,0xffffffff]
        for i in range(4000):
            n=rnd.randint(1,5);ph=b''
            for _ in range(n):
                t=rnd.choice([1,1,1,0,2,3,4,6,7,0x6474e551,0x70000001,9])
                f=[t]+[rnd.choice(vals+[rnd.getrandbits(32)]) for _ in range(5)]+[rnd.choice([4,5,6,7,1,0,8]),rnd.choice([0,1,4,4096,3,0x10000])]
                ph+=struct.pack('<8I',*f)
            hdr=b'\x7fELF\x01\x01\x01'+bytes(9)+struct.pack('<HHIIIIIHHHHHH',2,40,1,rnd.choice(vals),52,0,0x05000200,52,32,n,0,0,0)
            blob=(hdr+ph).ljust(rnd.choice([4096,0x3000,0x20000]),b'\0')
            r,p=self.check(blob)
            if r is None: accepted+=1;self.invariant(blob,p)
        self.assertGreater(accepted,-1)

class Space(Work):
    def setUp(self):
        super().setUp();self.assertEqual(self.c.us_reset(self.c.kernel_table()),0)
    def l1(self): return (C.c_uint32*4096).in_dll(self.c,'us_table')
    def pages(self):
        """Every user small page: (va, pa, AP, APX, XN)."""
        l1=self.l1();l2=((C.c_uint32*256)*16).in_dll(self.c,'us_l2');pa={self.c.us_l2_pa(j):j for j in range(16)};out=[]
        for i,e in enumerate(l1):
            if e&3==1:
                for k,d in enumerate(l2[pa[e&~0x3ff]]):
                    if d&2: out.append(((i<<20)|(k<<12),d&~0xfff,(d>>4)&3,(d>>9)&1,d&1))
        return out
    def test_descriptors(self):
        c=self.c
        self.assertEqual(c.us_map(IMAGE,0x2000,R|X),0);self.assertEqual(c.us_map(IMAGE+0x2000,0x1000,R),0)
        self.assertEqual(c.us_map(IMAGE+0x3000,0x3000,R|W),0);self.assertEqual(c.us_map(STACK_TOP-(1<<20),1<<20,R|W),0)
        got={va:(ap,apx,xn) for va,pa,ap,apx,xn in self.pages()}
        self.assertEqual(got[IMAGE],(2,1,0));self.assertEqual(got[IMAGE+0x1000],(2,1,0))   # text: read-only for both, executable
        self.assertEqual(got[IMAGE+0x2000],(2,1,1))                                       # rodata: read-only, never executable
        for va in (IMAGE+0x3000,IMAGE+0x5000,STACK_TOP-4096,STACK_TOP-(1<<20)): self.assertEqual(got[va],(3,0,1))   # data/stack: writable, XN
        self.assertNotIn(STACK_TOP-(1<<20)-4096,got);self.assertNotIn(STACK_TOP,got)       # guard below, reserved above
        for va,pa,*_ in self.pages(): self.assertTrue(POOL<=pa<POOL+POOL_BYTES)           # never kernel RAM or MMIO
        k=c.kernel_table().contents
        for i,e in enumerate(self.l1()):
            if e&3==2:   # every section is a kernel one: privileged only (AP=01, APX=0)
                self.assertEqual(e,k[i]);self.assertEqual(((e>>10)&3,(e>>15)&1),(1,0))
        self.assertEqual(sum(1 for e in self.l1() if e&3==1),3)
    def test_refusals(self):
        c=self.c
        self.assertEqual(c.us_map(IMAGE,4096,R|W|X),-EINVAL);self.assertEqual(text(c,'us_fail'),'USER MAP PROT')
        self.assertEqual(c.us_map(IMAGE+1,4096,R),-EINVAL);self.assertEqual(c.us_map(0x00100000,4096,R),-EINVAL)
        self.assertEqual(c.us_map(0x20000000,4096,R|W),-EINVAL);self.assertEqual(c.us_map(0x80000000,4096,R),-EINVAL)
        self.assertEqual(c.us_map(IMAGE,8192,R),0);self.assertEqual(c.us_map(IMAGE+4096,4096,R),-EINVAL)
        self.assertEqual(c.us_map(IMAGE+(1<<20),17<<20,R|W),-EINVAL)
        k=c.kernel_table().contents;k[0x080]=(0x080<<20)|0x402
        self.assertEqual(c.us_reset(c.kernel_table()),-EINVAL);self.assertEqual(text(c,'us_fail'),'USER MAP CONFLICT')
    def test_checks(self):
        c=self.c;c.us_map(IMAGE,4096,R|X);c.us_map(IMAGE+4096,8192,R|W)
        self.assertEqual(c.us_check(IMAGE+4096,8192,W),0);self.assertEqual(c.us_check(IMAGE,8192,R),0)   # spans two regions
        self.assertEqual(c.us_check(IMAGE,16,W),-EFAULT);self.assertEqual(c.us_check(IMAGE+4096,8193,W),-EFAULT)
        self.assertEqual(c.us_check(0xfffffff0,0x20,R),-EFAULT);self.assertEqual(c.us_check(IMAGE|(1<<32),4,R),-EFAULT)
        self.assertEqual(c.us_check(0x00100000,4,R),-EFAULT);self.assertEqual(c.us_check(IMAGE,1<<32,R),-EFAULT)
        self.assertEqual(c.us_check(IMAGE+4096,0,W),0)

class CallsBase(Work):
    """Records in user memory, dispatched as svc #0 would be (helpers only)."""
    REC=IMAGE+0x1000;BUF=IMAGE+0x2000
    def card(self,extra=None,order=0,fb=True):
        t={'MIT.TXT':(ROOT/'LICENSE').read_bytes(),'OS':{'DIAG.ELF':program(),**(extra or {})},'S512.BIN':bytes(range(256))*2}
        self.disk=self.work/f'card{id(self)}.img';fatgen.build(self.disk,t)
        self.c.open_disk(os.fsencode(self.disk));self.assertEqual(self.c.asset_mount(),0)
        self.hw=self.c.fb_setup(order,fb)
        if self.c.os_start(b'/OS/DIAG.ELF'): self.fail(self.c.take_log())
    def setUp(self):
        super().setUp();self.card();self.addCleanup(self.c.close_disk)
    def call(self,n,rec=None,size=None,major=1,extra=(0,0,0,0),va=None):
        va=self.REC if va is None else va
        if rec is not None: poke(self.c,va,rec)
        regs=(C.c_uint32*8)(va,len(rec) if size is None else size,major,*extra,n);return self.c.sys_call(C.byref(regs))
    @staticmethod
    def hdr(size,flags=0,r0=0,r1=0): return struct.pack('<4I',size,flags,r0,r1)
    def open(self,path,flags=0,mode=0,fd=0,at=None):
        if isinstance(path,str): path=path.encode()
        pva=self.BUF+0x800 if at is None else at
        if at is None: poke(self.c,pva,path)
        r=self.call(1,self.hdr(56)+struct.pack('<5Q',pva,len(path),flags,mode,fd))
        return r,struct.unpack_from('<Q',peek(self.c,self.REC,56),48)[0]
    def io(self,n,fd,buf,count,done=0): r=self.call(n,self.hdr(48)+struct.pack('<4Q',fd,buf,count,done));return r,struct.unpack_from('<Q',peek(self.c,self.REC,48),40)[0]
    def close(self,fd): return self.call(4,self.hdr(24)+struct.pack('<Q',fd))
    def ioctl(self,fd,req,buf,n): return self.call(5,self.hdr(48)+struct.pack('<4Q',fd,req,buf,n))
    def mmap(self,fd,length,prot=3,flags=1,offset=0): r=self.call(6,self.hdr(64)+struct.pack('<6Q',fd,offset,length,prot,flags,0));return r,struct.unpack_from('<Q',peek(self.c,self.REC,64),56)[0]

class Calls(CallsBase):
    def test_log_and_sd(self):
        r,log=self.open('/dev/log',1);self.assertEqual(r,0)
        poke(self.c,self.BUF,b'hello\x07 world\n');self.assertEqual(self.io(3,log,self.BUF,13),(0,13))
        self.assertIn(b'OS: hello? world\n',self.c.take_log())
        r,fd=self.open('/sd/S512.BIN');self.assertEqual(r,0)
        self.assertEqual(self.io(2,fd,self.BUF,600),(0,512));self.assertEqual(peek(self.c,self.BUF,512),bytes(range(256))*2)
        self.assertEqual(self.io(2,fd,self.BUF,600),(0,0))                     # end of file
        self.assertEqual(self.io(2,log,self.BUF,4)[0],-EBADF)                  # /dev/log is write-only
        self.assertEqual(self.io(3,fd,self.BUF,4)[0],-EBADF)                   # assets are read-only
        self.assertEqual(self.close(fd),0);self.assertEqual(self.close(fd),-EBADF)   # stale
        self.assertEqual(self.io(2,fd,self.BUF,4)[0],-EBADF)
        r,fd2=self.open('/sd/S512.BIN');self.assertNotEqual(fd2,fd)            # same slot, new generation
        r,root=self.open('/sd');self.assertEqual(r,0)
    def test_buffers_and_counts(self):
        r,fd=self.open('/sd/S512.BIN')
        for buf,count,want in ((IMAGE,4,-EFAULT),(0x00100000,4,-EFAULT),(0x20200000,4,-EFAULT),(0xfffff000,0x2000,-EFAULT),
                               (self.BUF|(1<<32),4,-EFAULT),(self.BUF+0x1f00,0x200,-EFAULT),(self.BUF,(1<<20)+1,-EINVAL),(self.BUF,1<<40,-EINVAL)):
            with self.subTest(hex(buf)): self.assertEqual(self.io(2,fd,buf,count)[0],want)
        for bad in (0,1,0x12345,1<<40,(99<<8)|0,(1<<8)|15,~0&(2**64-1)):
            with self.subTest(fd=bad): self.assertEqual(self.io(2,bad,self.BUF,4)[0],-EBADF)
    def test_record_rules(self):
        r,fd=self.open('/sd/S512.BIN');good=self.hdr(48)+struct.pack('<4Q',fd,self.BUF,4,0)
        self.assertEqual(self.call(2,good),0)
        self.assertEqual(self.call(2,good,major=2),-93)
        self.assertEqual(self.call(2,good,size=40),-EINVAL)
        self.assertEqual(self.call(2,self.hdr(48,flags=1)+good[16:]),-EINVAL)
        self.assertEqual(self.call(2,self.hdr(48,r0=1)+good[16:]),-EINVAL)
        self.assertEqual(self.call(2,self.hdr(48,r1=1)+good[16:]),-EINVAL)
        for i in range(4):
            e=[0,0,0,0];e[i]=1
            with self.subTest(reg=3+i): self.assertEqual(self.call(2,good,extra=e),-EINVAL)
        self.assertEqual(self.call(2,self.hdr(56)+good[16:]+bytes(8)),0)           # a newer minor's zero tail
        self.assertEqual(self.call(2,self.hdr(56)+good[16:]+b'\1'+bytes(7)),-EINVAL)
        self.assertEqual(self.call(2,self.hdr(48)+good[16:40]+struct.pack('<Q',5)),-EINVAL)   # output nonzero on entry
        self.assertEqual(self.call(2,good,va=self.REC+4),-EFAULT)                  # misaligned record
        self.assertEqual(self.call(2,None,size=48,va=0x00100000),-EFAULT)          # kernel record
        poke(self.c,IMAGE+0x100,good);self.assertEqual(self.call(2,None,size=48,va=IMAGE+0x100),-EFAULT)   # read-only record with outputs
        poke(self.c,IMAGE+0x100,self.hdr(24)+bytes(8));self.assertEqual(self.call(4,None,size=24,va=IMAGE+0x100),-EBADF)          # close reads only: text is fine to read
        for n in (0,7,8,15,24,99,0xffffffff): self.assertEqual(self.call(n,good),-ENOSYS)
        self.assertEqual(self.call(2,good,size=8),-EINVAL);self.assertEqual(self.call(2,good,size=8192),-EINVAL)
    def test_open_rules(self):
        self.assertEqual(self.open('/sd/NOPE.TXT')[0],-ENOENT);self.assertEqual(self.open('/x')[0],-ENOENT)
        self.assertEqual(self.open('/dev/nope')[0],-ENODEV);self.assertEqual(self.open('/sd/MIT.TXT',1)[0],-EROFS)
        self.assertEqual(self.open('/sd/MIT.TXT',3)[0],-EINVAL);self.assertEqual(self.open('/sd/MIT.TXT',0x200)[0],-EINVAL)
        self.assertEqual(self.open('/sd/MIT.TXT',0x100)[0],0);self.assertEqual(self.open('/sd/MIT.TXT',mode=1)[0],-EINVAL)
        self.assertEqual(self.open('/sd/MI\0T.TXT')[0],-EINVAL);self.assertEqual(self.open('/sd/'+'A'*253)[0],-ENAMETOOLONG)
        self.assertEqual(self.open('/dev/log',0)[0],-EACCES);self.assertEqual(self.open('/sd/MIT.TXT',fd=3)[0],-EINVAL)
        self.assertEqual(self.open(b'/sd/MIT.TXT',at=0x00100000)[0],-EFAULT)
        self.assertEqual(self.open(b'',at=self.BUF)[0],-EINVAL)
    def test_handle_quota(self):
        fds=[self.open('/dev/log',1) for _ in range(16)]
        self.assertTrue(all(r==0 for r,_ in fds));self.assertEqual(self.open('/dev/log',1)[0],-EMFILE)
        self.assertEqual(self.c.sys_open_handles(),16);self.c.sys_reset();self.assertEqual(self.c.sys_open_handles(),0)
        assets=[self.open('/sd/MIT.TXT')[0] for _ in range(9)];self.assertEqual(assets[:8],[0]*8);self.assertEqual(assets[8],-EMFILE)
    def test_framebuffer(self):
        r,fb=self.open('/dev/fb0',2);self.assertEqual(r,0);self.assertEqual(self.open('/dev/fb0',2)[0],-EBUSY)
        q=self.BUF;poke(self.c,q,self.hdr(56)+bytes(40))
        self.assertEqual(self.ioctl(fb,0x10000,q,56),0)
        self.assertEqual(struct.unpack_from('<5Q',peek(self.c,q,56),16),(640,480,2560,1,640*480*4))
        poke(self.c,q,self.hdr(48)+bytes(32));self.assertEqual(self.ioctl(fb,0x10000,q,48),-EINVAL)    # small query buffer
        poke(self.c,q,self.hdr(56)+bytes(40));self.assertEqual(self.ioctl(fb,0x10000,IMAGE,56),-EINVAL)  # text holds no valid record
        poke(self.c,IMAGE+0x200,self.hdr(56)+bytes(40));self.assertEqual(self.ioctl(fb,0x10000,IMAGE+0x200,56),-EFAULT)   # read-only output
        self.assertEqual(self.ioctl(fb,0x10009,q,56),-ENOTTY)
        r,log=self.open('/dev/log',1);self.assertEqual(self.ioctl(log,0x10000,q,56),-ENOTTY)
        poke(self.c,q,self.hdr(48)+struct.pack('<4Q',0,0,640,480));self.assertEqual(self.ioctl(fb,0x10001,q,48),-EINVAL)   # not mapped yet
        self.assertEqual(self.mmap(fb,640*480*4,prot=7)[0],-EACCES);self.assertEqual(self.mmap(fb,640*480*4,flags=2)[0],-EINVAL)
        self.assertEqual(self.mmap(fb,640*480*4,offset=4096)[0],-EINVAL);self.assertEqual(self.mmap(fb,(640*480*4)+4096)[0],-EINVAL)
        self.assertEqual(self.mmap(log,4096)[0],-ENODEV);self.assertEqual(self.mmap(2**64-1,4096)[0],-ENOSYS)
        self.assertEqual(self.mmap(fb,640*480*4),(0,FB_VA));self.assertEqual(self.mmap(fb,4096),(0,FB_VA))
        px=bytes(b for i in range(640*480) for b in struct.pack('<I',(i*2654435761)&0xffffff))
        poke(self.c,FB_VA,px)
        poke(self.c,q,self.hdr(48)+struct.pack('<4Q',0,0,640,480));self.assertEqual(self.ioctl(fb,0x10001,q,48),0)
        hw=C.string_at(self.hw,2700*480)
        for y in (0,1,240,479):
            self.assertEqual(hw[y*2700:y*2700+2560],px[y*2560:(y+1)*2560]);self.assertEqual(hw[y*2700+2560:y*2700+2700],b'\xaa'*140)   # pitch padding untouched
        for rect in ((1,0,640,1),(0,0,641,1),(0,480,1,1),(2**64-1,0,2,1),(0,2**63,1,2**63)):
            poke(self.c,q,self.hdr(48)+struct.pack('<4Q',*rect))
            with self.subTest(rect): self.assertEqual(self.ioctl(fb,0x10001,q,48),-EINVAL)
        poke(self.c,q,self.hdr(48)+struct.pack('<4Q',10,10,0,0));self.assertEqual(self.ioctl(fb,0x10001,q,48),0)
    def test_modes_and_palette(self):   # ABI 1.2
        r,fb=self.open('/dev/fb0',2);self.mmap(fb,640*480*4);q=self.BUF
        for m in ((960,540,4),(960,540,0),(1920,1080,2),(962,540,2),(8,8,3),(640,480,1)[:2]+(9,)):
            poke(self.c,q,self.hdr(40)+struct.pack('<3Q',*m))
            with self.subTest(m): self.assertEqual(self.ioctl(fb,0x10002,q,40),-EINVAL)
        poke(self.c,q,self.hdr(40)+struct.pack('<3Q',960,540,2));self.assertEqual(self.ioctl(fb,0x10002,q,40),0)
        poke(self.c,q,self.hdr(56)+bytes(40));self.assertEqual(self.ioctl(fb,0x10000,q,56),0)
        self.assertEqual(struct.unpack_from('<5Q',peek(self.c,q,56),16),(960,540,1920,2,960*540*2))
        px=bytes(random.Random(4).getrandbits(8) for _ in range(960*540*2));poke(self.c,FB_VA,px)
        poke(self.c,q,self.hdr(48)+struct.pack('<4Q',3,2,7,5));self.assertEqual(self.ioctl(fb,0x10001,q,48),0)   # odd x and width: byte edges
        hw=C.string_at(self.hw,1980*540)
        for y in range(2,7): self.assertEqual(hw[y*1980+6:y*1980+20],px[y*1920+6:y*1920+20])
        self.assertEqual(hw[2*1980+4:2*1980+6],b'\0\0');self.assertEqual(hw[2*1980+20:2*1980+22],b'\0\0')   # outside the rectangle
        poke(self.c,q,self.hdr(40)+struct.pack('<3Q',320,200,3));self.assertEqual(self.ioctl(fb,0x10002,q,40),0)
        poke(self.c,q,self.hdr(56)+bytes(40));self.ioctl(fb,0x10000,q,56)
        self.assertEqual(struct.unpack_from('<5Q',peek(self.c,q,56),16),(320,200,320,3,64000))
        poke(self.c,q,self.hdr(1056)+struct.pack('<2Q',254,2)+struct.pack('<2I',0x112233,0x445566)+bytes(1016))
        self.assertEqual(self.ioctl(fb,0x10003,q,1056),0)
        pal=(C.c_uint32*256).in_dll(self.c,'fb_palette');self.assertEqual((pal[254],pal[255]),(0x112233,0x445566))
        for first,count in ((0,0),(255,2),(256,1)):
            poke(self.c,q,self.hdr(1056)+struct.pack('<2Q',first,count)+bytes(1024))
            with self.subTest((first,count)): self.assertEqual(self.ioctl(fb,0x10003,q,1056),-EINVAL)
        f=C.c_uint32*3;mode=lambda:tuple(f.from_address(C.addressof(C.c_uint32.in_dll(self.c,'fb0'))))
        self.c.os_handle(5,0,0);drain(self.c);self.assertEqual(mode(),(640,480,32))   # a fault: the restart starts in 640x480x32
        self.c.os_handle(5,0,0);drain(self.c);self.assertEqual(mode(),(960,540,32))   # it stops: the kernel's own 16:9 screen ...
        self.assertEqual(C.string_at(self.hw,4),b'\xff\xff\xff\x00')                  # ... with its test pattern (the white border)
        self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'),0);self.assertEqual(mode(),(640,480,32))   # the next program starts in 640x480x32
    def test_rgb_and_no_display(self):
        self.c.os_handle(5,0,0);drain(self.c);self.c.os_handle(5,0,0);drain(self.c)   # end the program from setUp
        self.c.close_disk();self.card(order=1)   # firmware kept RGB bytes: present swaps
        r,fb=self.open('/dev/fb0',2);self.mmap(fb,640*480*4);poke(self.c,FB_VA,struct.pack('<I',0x112233))
        q=self.BUF;poke(self.c,q,self.hdr(48)+struct.pack('<4Q',0,0,1,1));self.assertEqual(self.ioctl(fb,0x10001,q,48),0)
        self.assertEqual(C.string_at(self.hw,4),struct.pack('<I',0x332211))
        self.c.os_handle(5,0,0);drain(self.c);self.c.os_handle(5,0,0);drain(self.c);self.c.close_disk();self.card(fb=False)
        self.assertEqual(self.open('/dev/fb0',2)[0],-ENODEV)
    def test_exit(self):
        self.assertEqual(self.call(16,self.hdr(24)+struct.pack('<q',-3)),1);self.assertEqual(C.c_int64.in_dll(self.c,'sys_exit_status').value,-3)

class SupervisorBase(CallsBase):
    def frame(self): return Frame.in_dll(self.c,'os_frame')
    def svc(self,pc_after,n,rec):
        f=self.frame();poke(self.c,self.REC,rec);f.r[0]=self.REC;f.r[1]=len(rec);f.r[2]=1;f.r[7]=n
        for i in (3,4,5,6): f.r[i]=0
        f.pc=pc_after;self.c.os_handle(1,0,0);return C.c_int32(self.frame().r[0]).value
    def stop_kinds(self,reason,fsr,far,kind):
        self.c.take_log();self.c.os_handle(reason,fsr,far);drain(self.c);log=self.c.take_log().decode()
        self.assertIn('OS fault '+kind,log);return log

class Supervisor(SupervisorBase,Calls):
    def test_entry_state(self):
        f=self.frame();self.assertEqual((f.pc,f.sp,f.lr,f.r[0],f.cpsr),(IMAGE,STACK_TOP,EXIT_VA,START_VA,0x10))
        size,major,minor,gen,trial,img,stack,handles=struct.unpack('<IHHIIQQQ',peek(self.c,START_VA,40))
        self.assertEqual((size,major,minor,img,stack,handles),(48,1,12,8<<20,1<<20,16))
        self.assertEqual(self.c.us_check(START_VA,40,W),-EFAULT)       # startup record is read-only
        self.assertIn(b'OS start /OS/DIAG.ELF entry 0x08000000 pages 3',self.c.take_log())
    def test_svc_paths(self):
        rec=self.hdr(56)+struct.pack('<5Q',self.BUF+0x800,8,1,0,0);poke(self.c,self.BUF+0x800,b'/dev/log')
        self.assertEqual(self.svc(IMAGE+4,1,rec),0)                     # svc #0 at IMAGE
        self.assertEqual(self.svc(IMAGE+8,1,rec),-ENOSYS)               # svc #1
        self.frame().cpsr=0x30;self.svc(IMAGE+4,1,rec);self.assertIn(b'OS fault thumb',self.c.take_log())
    def test_fault_restart_then_recovery(self):
        log=self.stop_kinds(5,0x80d,IMAGE,'data abort pc 0x08000000 fsr 0x0000080d addr 0x08000000')
        self.assertIn('OS restart',log);self.assertTrue(self.c.os_running());self.assertEqual(C.c_uint32.in_dll(self.c,'os_starts').value,2)
        self.assertEqual(self.c.sys_open_handles(),0)
        self.c.advance(5_000_000)
        log=self.stop_kinds(3,0,0,'undefined instruction');self.assertIn('kernel recovery console kept',log)
        self.assertFalse(self.c.os_running())
        hw=C.string_at(self.hw,4);self.assertEqual(hw,b'\xff\xff\xff\x00')   # HDMI shows the kernel screen again (white border)
        assets=[self.c.asset_open(b'/MIT.TXT',8,0) for _ in range(8)];self.assertTrue(all(a>=0 for a in assets))   # handles were revoked
    def test_restart_window(self):
        self.stop_kinds(4,5,0x20200000,'prefetch abort');self.assertTrue(self.c.os_running())
        self.c.advance(61_000_000);self.stop_kinds(5,0,0,'data abort');self.assertTrue(self.c.os_running())   # outside 60 s: restarted again
    def test_exits(self):
        f=self.frame();f.pc=EXIT_VA;f.r[0]=0;self.c.take_log();self.c.os_handle(4,0,EXIT_VA)
        self.assertIn(b'OS exit 0 /OS/DIAG.ELF; recovery console',self.c.take_log());self.assertFalse(self.c.os_running())
        drain(self.c);self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'),0)
        f=self.frame();poke(self.c,self.REC,self.hdr(24)+struct.pack('<q',7));f.r[0]=self.REC;f.r[1]=24;f.r[2]=1;f.r[7]=16;f.pc=IMAGE+4
        self.c.take_log();self.c.os_handle(1,0,0);log=self.c.take_log()
        self.assertIn(b'OS exit 7 counts as a fault',log);self.assertIn(b'OS restart',log)
    def test_screen_crc_logged(self):
        r,fb=self.open('/dev/fb0',2);self.mmap(fb,640*480*4);px=bytes(range(256))*(640*480*4//256);poke(self.c,FB_VA,px)
        q=self.BUF;poke(self.c,q,self.hdr(48)+struct.pack('<4Q',0,0,640,480));self.ioctl(fb,0x10001,q,48)
        want=zlib.crc32(b''.join(bytes(px[i+j] for j in range(3))+b'\0' for i in range(0,len(px),4)))
        f=self.frame();f.pc=EXIT_VA;f.r[0]=0;self.c.take_log();self.c.os_handle(4,0,EXIT_VA)
        self.assertIn(f'OS screen crc {want}'.encode(),self.c.take_log())
        self.assertEqual(C.string_at(self.hw,4),bytes(px[:3])+b'\0')   # a clean exit leaves the program's last frame
    def test_rejected_files(self):
        self.c.os_handle(5,0,0);drain(self.c);self.c.os_handle(5,0,0);drain(self.c);self.c.close_disk()
        bad={'WX.ELF':elf([(IMAGE,7,TEXT,16)]),'E64.ELF':elf([(IMAGE,5,TEXT,16)],ident=b'\x7fELF\x02\x01\x01'+bytes(9))}
        self.card(extra=bad);self.c.os_handle(5,0,0);drain(self.c);self.c.os_handle(5,0,0);drain(self.c);self.c.take_log()
        self.assertEqual(self.c.os_start(b'/OS/WX.ELF'),-8);self.assertIn(b'OS reject /OS/WX.ELF ELF WX',self.c.take_log())
        self.assertEqual(self.c.os_start(b'/OS/E64.ELF'),-8);self.assertEqual(self.c.os_start(b'/OS/NONE.ELF'),-ENOENT)
        self.assertFalse(self.c.os_running())

def boot_card(work,extra=None):
    folder=pathlib.Path(tempfile.mkdtemp(dir=work))
    known=image.pack(bytes(range(256))*4,'known')
    (folder/'KERNA.BIN').write_bytes(known.ljust(image.MAX_IMAGE,b'\0'));(folder/'KERNB.BIN').write_bytes(bytes(image.MAX_IMAGE))
    record=image.control_record(known)
    for n in (0,1): (folder/f'BOOTCT{n}.BIN').write_bytes(record+bytes(3584))
    (folder/'MIT.TXT').write_bytes((ROOT/'LICENSE').read_bytes());(folder/'config.txt').write_text('kernel=kernel.img\n')
    for k,v in (extra or {}).items(): (folder/k).write_bytes(v)
    disk=folder.with_suffix('.img');image.fat_image(folder,disk);return disk,folder

def fsck(disk,work):
    part=work/'part.img'
    with open(disk,'rb') as src,open(part,'wb') as dst:
        src.seek(2048*512);dst.truncate(262144*512)
        for i in range(0,262144*512,1<<20):
            b=src.read(1<<20)
            if b.strip(b'\0'): dst.seek(i);dst.write(b)
    checker=shutil.which('fsck_msdos') or shutil.which('fsck.fat') or shutil.which('fsck.vfat')
    if not checker: raise RuntimeError('Install fsck_msdos or dosfstools to check FAT images')
    try:
        return subprocess.run([checker,'-n',str(part)],capture_output=True,text=True)
    finally:
        part.unlink()

class InstallBase(Work):
    def setUp(self):
        super().setUp();self.disk,self.folder=boot_card(self.work)
        self.c.open_disk(os.fsencode(self.disk));self.addCleanup(self.c.close_disk);self.assertEqual(self.c.asset_mount(),0)
        self.before={p.name:p.read_bytes() for p in self.folder.iterdir()}
    def read(self,path):
        h=self.c.asset_open(path.encode(),len(path),0);self.assertGreaterEqual(h,0,text(self.c,'asset_fail'));out=b'';buf=C.create_string_buffer(65536)
        while (n:=self.c.asset_read(h,buf,65536))>0: out+=buf.raw[:n]
        self.c.asset_close(h);return out
    def fats(self):
        with open(self.disk,'rb') as f:
            f.seek((2048+32)*512);a=f.read(2048*512);b=f.read(2048*512)
        return a,b
    def free(self): a,_=self.fats();return sum(1 for i in range(2,len(a)//4) if not struct.unpack_from('<I',a,i*4)[0])
    def untouched(self):
        for name,data in self.before.items():
            with self.subTest(name): self.assertEqual(self.read('/'+name),data)
        a,b=self.fats();self.assertEqual(a,b)

class Install(InstallBase):
    def test_create_replace_and_checks(self):
        free=self.free();one=bytes(random.Random(2).getrandbits(8) for _ in range(5000))
        self.assertEqual(self.c.fatput(b'DIAG.ELF',one,len(one)),0,text(self.c,'fatput_fail'))
        self.assertEqual(self.read('/OS/DIAG.ELF'),one);self.assertEqual(self.free(),free-1-10)   # /OS's cluster + 10 data clusters
        two=b'second'*100;self.assertEqual(self.c.fatput(b'DIAG.ELF',two,len(two)),0)
        self.assertEqual(self.read('/OS/DIAG.ELF'),two);self.assertEqual(self.free(),free-1-2)    # old chain released
        self.assertEqual(self.c.fatput(b'LOOP.ELF',b'x',1),0);self.assertEqual(self.read('/OS/LOOP.ELF'),b'x')
        self.untouched()
        r=fsck(self.disk,self.work);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertNotIn('Fix',r.stdout,r.stdout)
    def test_app_folder_and_1_mib(self):
        js=b'console.log(1)\n';self.assertEqual(self.c.fatput(b'APPS/HELLO/MAIN.JS',js,len(js)),0,text(self.c,'fatput_fail'))
        self.assertEqual(self.read('/OS/APPS/HELLO/MAIN.JS'),js)
        self.assertEqual(self.c.fatput(b'APPS/HELLO/INDEX.HTM',b'<div>',5),0);self.assertEqual(self.read('/OS/APPS/HELLO/INDEX.HTM'),b'<div>')
        big=bytes(random.Random(3).getrandbits(8) for _ in range(1024*1024))
        self.assertEqual(self.c.fatput(b'JS.ELF',big,len(big)),0,text(self.c,'fatput_fail'));self.assertEqual(self.read('/OS/JS.ELF'),big)
        self.untouched()
        r=fsck(self.disk,self.work);self.assertEqual(r.returncode,0,r.stdout+r.stderr);self.assertNotIn('Fix',r.stdout,r.stdout)
    def test_bounded_reads(self):
        # The card's first 8,000+ clusters are in use (slots, firmware): the free
        # scan must read each FAT sector once, or the board's 8 s watchdog fires.
        r0=self.c.read_count();self.assertEqual(self.c.fatput(b'DIAG.ELF',b'x'*5000,5000),0)
        self.assertLess(self.c.read_count()-r0,600)
    def test_refusals(self):
        for name in (b'diag.elf',b'../X',b'/OS/X',b'A.B.C',b'TOOLONGNAME.ELF',b'X.ELFS',b'.ELF',b'',b'K RN.BIN'):
            with self.subTest(name): self.assertEqual(self.c.fatput(name,b'x',1),-EINVAL)
        self.assertEqual(self.c.fatput(b'BIG.ELF',bytes(1),1024*1024+1),-27)
        for name in (b'APPS/X',b'APPS/A.B/MAIN.JS',b'APPS/HELLO/X/Y',b'OS/APPS/HELLO/MAIN.JS'):
            with self.subTest(name): self.assertEqual(self.c.fatput(name,b'x',1),-EINVAL)
        self.assertEqual(self.c.fatput(b'KERNA.BIN',b'x',1),0)            # a same-named file lands in /OS, never the root
        self.untouched()
    def test_os_name_taken_by_a_file(self):
        self.c.close_disk();self.disk,self.folder=boot_card(self.work,{'OS':b'not a directory'})
        self.c.open_disk(os.fsencode(self.disk));self.c.asset_mount()
        self.assertEqual(self.c.fatput(b'DIAG.ELF',b'x',1),-20);self.assertEqual(text(self.c,'fatput_fail'),'PUT NOT DIR')
    def test_full_card(self):
        with open(self.disk,'r+b') as f:   # mark every free cluster bad
            for copy in (0,1):
                base=(2048+32+copy*2048)*512;f.seek(base);fat=bytearray(f.read(2048*512))
                for i in range(2,len(fat)//4):
                    if not struct.unpack_from('<I',fat,i*4)[0]: struct.pack_into('<I',fat,i*4,0x0ffffff7)
                f.seek(base);f.write(fat)
        self.assertEqual(self.c.fatput(b'DIAG.ELF',b'x',1),-28);self.assertEqual(text(self.c,'fatput_fail'),'PUT FULL')
        self.untouched()

class ProtocolBase(InstallBase):
    def run_protocol(self,limit=200000):
        out=b''
        for _ in range(limit):
            self.c.protocol_task();out+=self.c.take_output()
            if b'OK' in out or b'ERR' in out: break
        return out.decode()
    def put(self,data,name='DIAG.ELF',crc=None,sha=None):
        self.c.feed(b'INFO\n',5);self.run_protocol(3);self.c.take_output()
        line=f'PUTF {name} {len(data)} {crc if crc is not None else zlib.crc32(data):08x} {sha or hashlib.sha256(data).hexdigest()}\n'.encode()
        self.c.feed(line,len(line));out=self.run_protocol(50)
        if not out.startswith('READY'): return out
        self.c.feed(data,len(data));return out+self.run_protocol()
    def setUp(self):
        super().setUp();self.assertTrue(self.c.initialize());self.c.asset_mount()

class Protocol(ProtocolBase,Install):
    def test_put_and_run(self):
        out=self.put(program());self.assertIn('OK file /OS/DIAG.ELF',out);self.assertIn('ACK 1024',out)
        self.assertEqual(self.read('/OS/DIAG.ELF'),program())
        self.c.fb_setup(1,1);self.c.feed(b'RUN DIAG.ELF\n',13);self.assertIn('OK run /OS/DIAG.ELF',self.run_protocol())
        self.assertTrue(self.c.os_running())
        self.assertIn('ERR not-ready',self.put(b'x'))                  # never while the OS runs
        self.c.feed(b'RUN DIAG.ELF\n',13);self.assertIn('ERR not-ready',self.run_protocol())
        self.untouched()
    def test_put_refusals(self):
        self.assertIn('ERR sha256',self.put(b'hello',sha='0'*64))
        self.assertIn('ERR crc',self.put(b'hello',crc=1))
        self.assertIn('ERR command',self.put(b'hello',name='diag.elf'))
        self.assertIn('ERR command',self.put(b'hello',name='../BOOT'))
        self.assertIn('ERR size',self.put(bytes(1024*1024+1)))
        self.c.feed(b'RUN NOPE.ELF\n',13);self.assertIn('ERR run ASSET NOT FOUND',self.run_protocol())
        self.untouched()

if __name__=='__main__': unittest.main()
