# SPDX-License-Identifier: MIT
"""M1 slice 1 host tests: read-only FAT32 assets (acceptance test 7) and the
framebuffer reply validator and test pattern. Real core code, fake media."""
import ctypes as C
import hashlib, os, pathlib, random, shutil, struct, subprocess, sys, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'tests'))
import fatgen
ENOENT,EIO,EBADF,ENOTDIR,EINVAL,EMFILE,EROFS,ENAMETOOLONG,ENODEV=2,5,9,20,22,24,30,36,19

class Dirent(C.Structure):
    _fields_=[('name',C.c_char*13),('attr',C.c_uint8),('is_dir',C.c_uint8),('reserved',C.c_uint8),('size',C.c_uint32)]
class FbInfo(C.Structure):
    _fields_=[(n,C.c_uint32) for n in ('width','height','depth','pitch','bytes','pixel_order','bus','phys')]

def build_lib(work):
    lib=work/'m1.dylib'
    import hostlib;hostlib.build(lib)
    return lib

def data(n,seed=1):
    r=random.Random(seed);return bytes(r.getrandbits(8) for _ in range(n))

SIZES=[0,1,511,512,513,1023,1024,1025,4096,70001]
def tree():
    t={f'S{n}.BIN':data(n,n) for n in SIZES}
    t['EMPTY.TXT']=b''
    t['GAMES']={'ONE.GAM':data(3000,7),'DEEP':{'LEAF.DAT':data(600,9)},'NOTHING':{}}
    t['BIG.BIN']=data(1024*1024+700,3)
    return t

class Assets(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.work=pathlib.Path(cls.temp.name);cls.lib=build_lib(cls.work);cls.n=0
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def load(self,spc=1,fragment=False,t=None,mount=True):
        Assets.n+=1;d=self.work/f'case{self.n}';d.mkdir()
        shutil.copyfile(self.lib,d/'lib.dylib');c=C.CDLL(str(d/'lib.dylib'))
        self.card=d/'card.img';self.tree=t if t is not None else tree()
        self.vol=fatgen.build(self.card,self.tree,spc,fragment)
        c.open_disk.argtypes=[C.c_char_p];c.asset_open.argtypes=[C.c_char_p,C.c_uint32,C.c_uint32]
        c.asset_read.argtypes=[C.c_int,C.c_void_p,C.c_uint32];c.asset_readdir.argtypes=[C.c_int,C.c_void_p]
        c.asset_fail_text=C.c_char_p.in_dll(c,'asset_fail')
        self.c=c;self.addCleanup(self.finish)
        if mount: c.open_disk(os.fsencode(self.card));self.assertEqual(c.asset_mount(),0)
        self.hash=None
        return c
    def finish(self):
        self.assertEqual(self.c.write_count(),0,'asset reader wrote to the card')
        self.c.close_disk()
    def fail_code(self): return C.c_char_p.in_dll(self.c,'asset_fail').value.decode()
    def open(self,path,flags=0):
        b=path.encode() if isinstance(path,str) else path
        return self.c.asset_open(b,len(b),flags)
    def read_all(self,h,chunk):
        out=bytearray();buf=C.create_string_buffer(max(chunk,1))
        while True:
            n=self.c.asset_read(h,buf,chunk)
            self.assertGreaterEqual(n,0,self.fail_code())
            if not n: return bytes(out)
            self.assertLessEqual(n,chunk);out+=buf.raw[:n]
    def listing(self,path):
        h=self.open(path);self.assertGreaterEqual(h,0,self.fail_code());out=[];e=Dirent()
        while (r:=self.c.asset_readdir(h,C.byref(e)))==1: out.append((e.name.decode(),e.is_dir,e.size))
        self.assertEqual(r,0,self.fail_code());self.assertEqual(self.c.asset_close(h),0);return out

    def test_boundary_reads_every_chunk(self):
        for spc,frag in ((1,False),(2,False),(1,True),(2,True)):
            with self.subTest(spc=spc,fragment=frag):
                self.load(spc,frag)
                for n in SIZES:
                    for chunk in (1,511,512,513,1024,4096,1<<20):
                        if n>5000 and chunk<512: continue
                        h=self.open(f'/S{n}.BIN');self.assertGreaterEqual(h,0,self.fail_code())
                        got=self.read_all(h,chunk);self.assertEqual(got,self.tree[f'S{n}.BIN'],(n,chunk))
                        self.assertEqual(self.c.asset_read(h,C.create_string_buffer(8),8),0)
                        self.assertEqual(self.c.asset_close(h),0)
                self.finish();self._cleanups.clear()
    def test_empty_file_and_zero_count(self):
        self.load();h=self.open('/EMPTY.TXT');self.assertGreaterEqual(h,0)
        size=C.c_uint32(9);self.assertEqual(self.c.asset_size(h,C.byref(size)),0);self.assertEqual(size.value,0)
        self.assertEqual(self.c.asset_read(h,C.create_string_buffer(4),4),0)
        h2=self.open('/S512.BIN');self.assertEqual(self.c.asset_read(h2,None,0),0)
    def test_transfer_limit(self):
        self.load();h=self.open('/BIG.BIN');buf=C.create_string_buffer((1<<20)+1)
        self.assertEqual(self.c.asset_read(h,buf,(1<<20)+1),-EINVAL)
        self.assertEqual(self.c.asset_read(h,buf,1<<20),1<<20)
        self.assertEqual(self.c.asset_read(h,buf,1<<20),700)
        self.assertEqual(buf.raw[:700],self.tree['BIG.BIN'][1<<20:])
    def test_directories(self):
        self.load()
        root=self.listing('/')
        self.assertEqual([n for n,_,_ in root],[f'S{n}.BIN' for n in SIZES]+['EMPTY.TXT','GAMES','BIG.BIN'])
        self.assertIn(('GAMES',1,0),root);self.assertIn(('S513.BIN',0,513),root)
        self.assertEqual(self.listing('/games'),[('ONE.GAM',0,3000),('DEEP',1,0),('NOTHING',1,0)])
        self.assertEqual(self.listing('/GAMES/NOTHING/'),[])
        h=self.open('/Games/Deep/leaf.dat');self.assertEqual(self.read_all(h,100),self.tree['GAMES']['DEEP']['LEAF.DAT'])
    def test_large_directory_spans_clusters(self):
        t={f'F{i}.TXT':bytes([i%256])*(i%5) for i in range(100)}
        self.load(t=t);names=self.listing('/');self.assertEqual(len(names),100)
        self.assertGreater(len(self.vol.chains['/']),1)
        h=self.open('/F99.TXT');self.assertEqual(self.read_all(h,10),t['F99.TXT'])
    def test_path_errors(self):
        self.load()
        cases={'/NOPE.TXT':-ENOENT,'/NODIR/X.TXT':-ENOENT,'/S1.BIN/X':-ENOTDIR,'/S1.BIN/':-ENOTDIR,
               'S1.BIN':-EINVAL,'':-EINVAL,'/GAMES/../S1.BIN':-EINVAL,'/./S1.BIN':-EINVAL,'/GAMES//ONE.GAM':-EINVAL,'//':-EINVAL,
               '/a*b':-EINVAL,'/LONGFILENAME.TXT':-ENOENT,'/A.LONG':-ENOENT,'/café.txt':-ENOENT,'/A B':-ENOENT,
               '/'+'X/'*20+'Y':-ENAMETOOLONG,'/'+'A'*300:-ENAMETOOLONG}
        for p,want in cases.items():
            with self.subTest(path=p): self.assertEqual(self.open(p),want,self.fail_code())
        self.assertEqual(self.c.asset_open(b'/S1.BIN\0x',9,0),-EINVAL)
        self.assertEqual(self.c.asset_open(None,0,0),-EINVAL)
        self.assertEqual(self.open('/S1.BIN',1),-EROFS);self.assertEqual(self.open('/S1.BIN',2),-EROFS)
        self.assertEqual(self.open('/S1.BIN',0x100),-EINVAL)
    def test_handles(self):
        self.load();hs=[self.open('/S1.BIN') for _ in range(8)]
        self.assertTrue(all(h>=0 for h in hs));self.assertEqual(self.open('/S1.BIN'),-EMFILE)
        self.assertEqual(self.c.asset_close(hs[3]),0);self.assertEqual(self.c.asset_close(hs[3]),-EBADF)
        buf=C.create_string_buffer(4)
        self.assertEqual(self.c.asset_read(hs[3],buf,1),-EBADF)
        again=self.open('/S1.BIN');self.assertGreaterEqual(again,0);self.assertNotEqual(again,hs[3])
        self.assertEqual(again&7,hs[3]&7);self.assertEqual(self.c.asset_read(hs[3],buf,1),-EBADF)
        for bad in (-1,-EBADF,0,1<<30): self.assertEqual(self.c.asset_read(bad,buf,1),-EBADF)
        self.assertEqual(self.c.asset_close(again),0);d=self.open('/GAMES');e=Dirent();self.assertGreaterEqual(d,0)
        self.assertEqual(self.c.asset_read(d,buf,1),-EINVAL);self.assertEqual(self.c.asset_readdir(hs[0],C.byref(e)),-ENOTDIR)
        self.assertEqual(self.c.asset_mount(),0);self.assertEqual(self.c.asset_read(hs[0],buf,1),-EBADF)
    def corrupt(self,path,how,expect,spc=1):
        self.load(spc);v=self.vol
        how(v)
        got=self.open(path)
        self.assertEqual(got,-expect,f'{self.fail_code()}');return self.fail_code()
    def test_corrupt_fat_chains(self):
        c=lambda p:self.vol.chains[p]
        cases=[('free',lambda v:fatgen.set_fat(self.card,v,c('/S4096.BIN')[2],0),'FAT FREE'),
               ('bad',lambda v:fatgen.set_fat(self.card,v,c('/S4096.BIN')[1],0x0ffffff7),'FAT BAD'),
               ('range',lambda v:fatgen.set_fat(self.card,v,c('/S4096.BIN')[1],v.clusters+5),'FAT BAD'),
               ('short',lambda v:fatgen.set_fat(self.card,v,c('/S4096.BIN')[3],fatgen.EOC),'FAT SHORT'),
               ('long',lambda v:fatgen.set_fat(self.card,v,c('/S4096.BIN')[-1],c('/S513.BIN')[0]),'FAT LONG'),
               ('loop',lambda v:fatgen.set_fat(self.card,v,c('/S4096.BIN')[-1],c('/S4096.BIN')[0]),'FAT LONG'),
               ('self',lambda v:fatgen.set_fat(self.card,v,c('/S4096.BIN')[0],c('/S4096.BIN')[0]),'FAT LONG')]
        for name,how,code in cases:
            with self.subTest(name):
                self.assertEqual(self.corrupt('/S4096.BIN',how,EIO),code);self.finish();self._cleanups.clear()
    def test_corrupt_entries(self):
        def entry(path,off,data): return lambda v:fatgen.patch(self.card,v.entry_offset(path)+off,data)
        cases=[('cluster zero',entry('/S513.BIN',26,b'\0\0'),'/S513.BIN','FILE CLUSTER'),
               ('cluster range',entry('/S513.BIN',20,b'\xff\x0f'),'/S513.BIN','FILE CLUSTER'),
               ('too big',entry('/S513.BIN',28,struct.pack('<I',(64<<20)+1)),'/S513.BIN','FILE TOO BIG'),
               ('size beyond chain',entry('/S513.BIN',28,struct.pack('<I',5000)),'/S513.BIN','FAT SHORT'),
               ('size under chain',entry('/S1025.BIN',28,struct.pack('<I',100)),'/S1025.BIN','FAT LONG'),
               ('attr',entry('/S1.BIN',11,b'\xe0'),'/S513.BIN','DIR ATTR'),
               ('name',entry('/S1.BIN',3,b'\x01'),'/S513.BIN','DIR NAME'),
               ('dir cluster',entry('/GAMES',26,b'\0\0'),'/GAMES/ONE.GAM','DIR ENTRY CLUSTER')]
        for name,how,path,code in cases:
            with self.subTest(name):
                self.assertEqual(self.corrupt(path,how,EIO),code);self.finish();self._cleanups.clear()
    def test_directory_loop_is_bounded(self):
        t={f'F{i}.TXT':b'x' for i in range(40)};self.load(t=t);ch=self.vol.chains['/']
        self.assertGreaterEqual(len(ch),2);fatgen.set_fat(self.card,self.vol,ch[-1],ch[0])
        used=3+len(t) # label, deleted and long-name entries come first
        for i in range(used,len(ch)*16): fatgen.patch(self.card,self.vol.lba(ch[i//16])*512+i%16*32,b'\xe5') # no end marker
        self.assertEqual(self.open('/MISSING.TXT'),-EIO);self.assertEqual(self.fail_code(),'DIR LOOP')
        h=self.open('/');e=Dirent();n=0
        while (r:=self.c.asset_readdir(h,C.byref(e)))==1: n+=1
        self.assertEqual(r,-EIO);self.assertLess(n,65536)
    def test_corruption_after_open_gives_short_read_then_error(self):
        self.load();ch=self.vol.chains['/S4096.BIN'];h=self.open('/S4096.BIN')
        fatgen.set_fat(self.card,self.vol,ch[2],0)
        buf=C.create_string_buffer(4096)
        self.assertEqual(self.c.asset_read(h,buf,4096),1536);self.assertEqual(buf.raw[:1536],self.tree['S4096.BIN'][:1536])
        self.assertEqual(self.c.asset_read(h,buf,4096),-EIO)
    def test_bad_media(self):
        bad={'mbr sig':(510,b'\0\0','ASSET MBR SIG',EIO),'mbr type':(450,b'\x83','ASSET MBR TYPE',ENODEV),
             'mbr range':(458,struct.pack('<I',300000),'ASSET MBR RANGE',EIO),
             'sector size':(2048*512+11,struct.pack('<H',1024),'ASSET BPB',EIO),
             'one fat':(2048*512+16,b'\x01','ASSET BPB',EIO),'spc':(2048*512+13,b'\x03','ASSET BPB GEOM',EIO),
             'spc fat16 size':(2048*512+13,b'\x08','ASSET BPB CLUSTERS',EIO),
             'root':(2048*512+44,struct.pack('<I',1),'ASSET ROOT',EIO),
             'total':(2048*512+32,struct.pack('<I',300000),'ASSET BPB GEOM',EIO)}
        for name,(off,b,code,err) in bad.items():
            with self.subTest(name):
                self.load(mount=False);fatgen.patch(self.card,off,b);self.c.open_disk(os.fsencode(self.card))
                self.assertEqual(self.c.asset_mount(),-err);self.assertEqual(self.fail_code(),code)
                self.assertEqual(self.open('/S1.BIN'),-ENODEV)
                self.finish();self._cleanups.clear()
    def test_missing_media(self):
        self.load(mount=False)
        with open(self.card,'r+b') as f: f.truncate(2048*512)
        self.c.open_disk(os.fsencode(self.card))
        self.assertEqual(self.c.asset_mount(),-EIO);self.assertEqual(self.fail_code(),'ASSET SD READ')
    def test_card_unchanged(self):
        self.load(1,True);before=hashlib.sha256(self.card.read_bytes()).digest()
        for p in ('/','/GAMES','/GAMES/DEEP'): self.listing(p)
        for n in SIZES: h=self.open(f'/S{n}.BIN');self.read_all(h,777);self.assertEqual(self.c.asset_close(h),0)
        self.assertEqual(hashlib.sha256(self.card.read_bytes()).digest(),before)

class Framebuffer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();cls.c=C.CDLL(str(build_lib(pathlib.Path(cls.temp.name))))
        cls.c.fb_parse.restype=C.c_char_p;cls.c.fb_pattern_pixel.restype=C.c_uint32
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def reply(self,w=640,h=480,depth=32,pitch=2560,bus=0xde800000,size=None,order=0):
        m=(C.c_uint32*36)();self.c.fb_request(m,640,480,32)
        self.assertEqual(list(m[:7]),[144,0,0x48003,8,8,640,480]);self.assertEqual(m[34],0)
        self.assertEqual([m[i] for i in (17,21,25,30)],[0x48005,0x48006,0x40001,0x40008])
        self.assertEqual(m[24],0)   # BGR bytes: the word 0x00RRGGBB shows as written (HDMI, 2026-11-07)
        m[1]=0x80000000
        for t in (2,7,12,17,21,25,30): m[t+2]=0x80000000|m[t+1]
        m[5],m[6],m[10],m[11]=w,h,w,h;m[20]=depth;m[24]=order;m[28]=bus;m[29]=pitch*h if size is None else size;m[33]=pitch
        return m
    def parse(self,m):
        f=FbInfo();r=self.c.fb_parse(m,32,C.byref(f));return (r.decode() if r else None),f
    def test_good_reply(self):
        r,f=self.parse(self.reply(pitch=2560+64))
        self.assertIsNone(r);self.assertEqual((f.width,f.height,f.depth,f.pitch,f.phys,f.bus),(640,480,32,2624,0x1e800000,0xde800000))
    def test_bad_replies(self):
        cases={'FB RESP':lambda m:m.__setitem__(1,0x80000001),'FB TAG':lambda m:m.__setitem__(27,8),
               'FB SIZE':lambda m:m.__setitem__(10,800),'FB DEPTH':lambda m:m.__setitem__(20,16),
               'FB PITCH':lambda m:m.__setitem__(33,100),'FB ALLOC':lambda m:m.__setitem__(28,0),
               'FB BYTES':lambda m:m.__setitem__(29,1000),'FB RANGE':lambda m:m.__setitem__(28,0x40100000)}
        for code,how in cases.items():
            with self.subTest(code):
                m=self.reply();how(m);self.assertEqual(self.parse(m)[0],code)
        self.assertEqual(self.parse(self.reply(w=8))[0],'FB SIZE')
        self.assertEqual(self.parse(self.reply(bus=0xdffff000,size=0x200000))[0],'FB RANGE')
        m=self.reply();m[30]=0x48008;self.assertEqual(self.parse(m)[0],'FB TAG')
        m=self.reply();m[0]=140;self.assertEqual(self.parse(m)[0],'FB RESP')
    def test_pattern(self):
        f=FbInfo(64,48,32,64*4+32,0,0,0,0);buf=(C.c_uint8*(f.pitch*48))(*([0xaa]*(f.pitch*48)))
        self.c.fb_pattern(C.byref(f),buf);px=lambda x,y:struct.unpack_from('<I',bytes(buf),y*f.pitch+x*4)[0]
        self.assertEqual(px(0,0),0xffffff);self.assertEqual(px(63,47),0xffffff)
        self.assertEqual([px(x*8+3,10) for x in range(8)],[0xffffff,0xffff00,0x00ffff,0x00ff00,0xff00ff,0xff0000,0x0000ff,0])
        self.assertEqual(px(4,40),0x111111);self.assertEqual(px(58,40),0xffffff-0x111111)
        self.assertTrue(all(b==0xaa for b in bytes(buf)[256:288]),'pitch padding was written')
        f.pixel_order=1;self.assertEqual(self.c.fb_pattern_pixel(C.byref(f),44,10),0x0000ff)   # RGB kept: red bar swapped
