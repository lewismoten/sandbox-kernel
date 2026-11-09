# SPDX-License-Identifier: MIT
"""The e-ink API (ABI 1.9, docs/eink.md): /dev/eink title and page, bit order, the limits (EAGAIN),
kernel reclaim on fault and exit, and a kernel error winning over an OS page. Real core code
through the syscall dispatcher over fake media (tests/harness.c)."""
import ctypes as C
import struct, unittest
from test_m1_user import SupervisorBase, poke, drain
from test_boot_screens import font, text as ktext
EAGAIN,EBUSY,EINVAL,EACCES,EBADF,ENOTTY=11,16,22,13,9,25
WR=1
Q,TITLE,CLEAR=0x40000,0x40001,0x40002
PAGE=3904
def plane_pixels(plane):
    return {(lx,ly) for ly in range(122) for lx in range(250) if not plane[lx*16+(121-ly)//8]&(0x80>>((121-ly)%8))}
def page_with(pixels):
    b=bytearray(PAGE)
    for x,y in pixels: b[y*32+x//8]|=0x80>>(x%8)
    return bytes(b)
def mark_with(pixels):
    b=bytearray(200)
    for x,y in pixels: b[y*5+x//8]|=0x80>>(x%8)
    return bytes(b)

class Eink(SupervisorBase):
    BUFP=SupervisorBase.BUF
    def dev(self,flags=WR):
        r,fd=self.open('/dev/eink',flags);return r,fd
    def ready(self):
        r,fd=self.dev();self.assertEqual(r,0);return fd
    def write_page(self,fd,data):
        poke(self.c,self.BUFP,data);return self.io(3,fd,self.BUFP,len(data))[0]
    def title(self,fd,name,mark=None,flags=None,tail=b''):
        n=name if isinstance(name,bytes) else name.encode()
        rec=self.hdr(248)+struct.pack('<QQ',(1 if mark else 0) if flags is None else flags,len(n))+(n+tail).ljust(16,b'\0')+(mark or bytes(200))
        poke(self.c,self.BUFP,rec);return self.ioctl(fd,TITLE,self.BUFP,248)
    def clear(self,fd):
        poke(self.c,self.BUFP,self.hdr(16));return self.ioctl(fd,CLEAR,self.BUFP,16)
    def screen(self,error=b'none'):
        buf=(C.c_uint8*4000)();self.c.eink_compose(buf,b'5CA68D431228',b'enumerated',error,C.c_uint32(60));return plane_pixels(bytes(buf))
    def wait(self,us=1_100_000): self.c.advance(us)
    def kernel_screen(self,error=b'none'):
        return self.screen_of(None,error)
    def test_query(self):
        fd=self.ready();poke(self.c,self.BUFP,self.hdr(80)+bytes(64))
        self.assertEqual(self.ioctl(fd,Q,self.BUFP,80),0)
        self.assertEqual(struct.unpack_from('<8Q',self.peek(self.BUFP,80),16),(250,122,3904,16,200,1000,30,0))
    def peek(self,va,n):
        from test_m1_user import peek
        return peek(self.c,va,n)
    def test_title_set_and_clear(self):
        base=self.screen();fd=self.ready()
        mark=mark_with([(0,0),(39,39),(20,20)])
        self.assertEqual(self.title(fd,'scoot',mark),0)                    # lowercase is drawn as capitals
        got=self.screen()
        self.assertIn((4,2),got);self.assertIn((4+39,2+39),got)             # the OS mark sits where the chip mark did
        self.assertTrue(ktext(54,6,'SCOOT')<=got)
        self.assertFalse(ktext(54,6,'SANDBOX KERNEL')&got-ktext(54,6,'SCOOT'))
        self.assertTrue(ktext(4,48,'UP:')<=got and ktext(4,92,'ERR:')<=got and ktext(54,92,'NONE')<=got)   # the kernel's lines stay
        self.wait();self.assertEqual(self.clear(fd),0);self.assertEqual(self.screen(),base)
    def test_title_without_mark_keeps_the_chip_mark_off_and_16_chars_fit(self):
        fd=self.ready();self.assertEqual(self.title(fd,'ABCDEFGHIJKLMNOP'),0)
        self.assertTrue(ktext(54,6,'ABCDEFGHIJKLMNOP')<=self.screen())
    def test_page_bit_order_and_render(self):
        fd=self.ready();px={(0,0),(9,3),(249,121),(8,1),(7,0),(100,60)}
        self.assertEqual(self.write_page(fd,page_with(px)),0)
        self.assertEqual(self.screen(),px)                                  # 1 = black, bit 7 = leftmost, rows of 32 bytes
        self.wait();raw=bytearray(PAGE);raw[0]=0x80;raw[1]=0x40;raw[32]=0x01   # (0,0) (9,0) (7,1)
        self.assertEqual(self.write_page(fd,bytes(raw)),0);self.assertEqual(self.screen(),{(0,0),(9,0),(7,1)})
        self.wait();pad=bytearray(PAGE);pad[31]=0x3f                       # the 6 padding bits of a row are ignored
        self.assertEqual(self.write_page(fd,bytes(pad)),0);self.assertEqual(self.screen(),set())
    def test_page_covers_title(self):
        fd=self.ready();self.title(fd,'SCOOT');self.wait();self.write_page(fd,page_with({(5,5)}))
        self.assertEqual(self.screen(),{(5,5)})
    def test_bad_sizes_and_records(self):
        fd=self.ready();self.wait()
        for n in (0,1,3903,3905,4096,32):
            with self.subTest(n): self.assertEqual(self.write_page(fd,bytes(n)),-EINVAL)
        self.assertEqual(self.screen(),self.screen());self.assertEqual(self.write_page(fd,bytes(PAGE)),0)   # refusals never used the budget
        self.wait()
        for name in (b'',b'A'*17,b'HI@',b'\xc3\xa9'):
            with self.subTest(name): self.assertEqual(self.title(fd,name,flags=0),-EINVAL)
        self.assertEqual(self.title(fd,'OK',flags=2),-EINVAL)                # unknown flag
        self.assertEqual(self.title(fd,'OK',tail=b'X'),-EINVAL)              # bytes after name_bytes must be zero
        self.wait();bad=self.hdr(248)+struct.pack('<QQ',0,2)+b'OK'.ljust(16,b'\0')+b'\1'+bytes(199)
        poke(self.c,self.BUFP,bad);self.assertEqual(self.ioctl(fd,TITLE,self.BUFP,248),-EINVAL)   # mark bytes without the flag
        poke(self.c,self.BUFP,self.hdr(240)+bytes(224));self.assertEqual(self.ioctl(fd,TITLE,self.BUFP,240),-EINVAL)   # wrong record size
        poke(self.c,self.BUFP,self.hdr(16));self.assertEqual(self.ioctl(fd,0x40003,self.BUFP,16),-ENOTTY)
    def test_device_rules(self):
        self.assertEqual(self.dev(0)[0],-EACCES);self.assertEqual(self.dev(2)[0],-EACCES)   # write-only
        r,fd=self.dev();self.assertEqual(r,0);self.assertEqual(self.dev()[0],-EBUSY)         # one handle
        self.assertEqual(self.io(2,fd,self.BUFP,PAGE)[0],-EBADF)
        self.assertEqual(self.close(fd),0);r,fd2=self.dev();self.assertEqual(r,0)
        self.assertEqual(self.mmap(fd2,4096)[0]!=0,True)
    def test_close_keeps_the_page(self):
        fd=self.ready();self.write_page(fd,page_with({(1,1)}));self.close(fd);self.assertEqual(self.screen(),{(1,1)})
    def test_rate_limits(self):
        fd=self.ready();self.assertEqual(self.write_page(fd,page_with({(1,1)})),0)
        self.assertEqual(self.write_page(fd,page_with({(2,2)})),-EAGAIN)    # under 1 s: dropped
        self.assertEqual(self.screen(),{(1,1)})
        self.c.advance(900_000);self.assertEqual(self.write_page(fd,page_with({(2,2)})),-EAGAIN)   # still under 1 s since the first
        self.c.advance(100_000);self.assertEqual(self.write_page(fd,page_with({(2,2)})),0)         # exactly 1 s
        self.assertEqual(self.title(fd,'X'),-EAGAIN);self.assertEqual(self.clear(fd),-EAGAIN)      # titles and clears count too
        done=2
        while True:
            self.c.advance(1_100_000);r=self.write_page(fd,page_with({(3,3)}))
            if r: break
            done+=1
        self.assertEqual((r,done),(-EAGAIN,30))                              # 30 in a minute, then EAGAIN
        self.c.advance(30_000_000);self.assertEqual(self.write_page(fd,page_with({(4,4)})),0)      # the window has moved on
    def test_full_refresh_schedule(self):
        c=self.c;c.eink_full_due.restype=C.c_bool
        for _ in range(49): c.eink_note_refresh(False,C.c_uint64(1))
        self.assertFalse(c.eink_full_due(C.c_uint64(10**9)))
        c.eink_note_refresh(False,C.c_uint64(1));self.assertTrue(c.eink_full_due(C.c_uint64(10**9)))
        c.eink_note_refresh(True,C.c_uint64(10**9));self.assertFalse(c.eink_full_due(C.c_uint64(10**9)))   # counter restarts
        for _ in range(50): c.eink_note_refresh(False,C.c_uint64(10**9+1))
        self.assertFalse(c.eink_full_due(C.c_uint64(10**9+179_000_000)))   # not sooner than 180 s after the last full
        self.assertTrue(c.eink_full_due(C.c_uint64(10**9+180_000_000)))
    def test_kernel_reclaims_on_fault(self):
        fd=self.ready();self.title(fd,'SCOOT');self.wait();self.write_page(fd,page_with({(5,5)}))
        self.c.os_handle(5,0,0);drain(self.c)                                # a data abort
        self.assertTrue(ktext(54,6,'SANDBOX KERNEL')<=self.screen())
    def test_kernel_reclaims_on_clean_exit(self):
        fd=self.ready();self.title(fd,'SCOOT');self.wait();self.write_page(fd,page_with({(5,5)}))
        self.svc(0x08000004,16,self.hdr(24)+struct.pack('<q',0));drain(self.c)
        self.assertTrue(ktext(54,6,'SANDBOX KERNEL')<=self.screen())
    def test_kernel_error_wins_over_a_page(self):
        fd=self.ready();self.write_page(fd,page_with({(5,5)}))
        self.assertEqual(self.screen(),{(5,5)});self.assertEqual(self.screen(b'none'),{(5,5)})
        err=self.screen(b'SD READ FAIL')                                    # the very next update is the kernel's screen
        self.assertTrue(ktext(54,92,'SD READ FAIL')<=err);self.assertNotIn((5,5),err)
        self.assertEqual(self.screen(b'SD READ FAIL')&{(5,5)},set())        # and the page stays away until the OS submits again
        self.wait();self.write_page(fd,page_with({(5,5)}))
        corner={(x,y) for x in range(242,250) for y in range(8)}
        self.assertEqual(self.screen(b'SD READ FAIL'),{(5,5)}|corner)        # now with the kernel's indicator
        self.assertEqual(self.screen(b'none'),{(5,5)})                      # no indicator without an error
    def test_a_page_cannot_hide_the_indicator(self):
        fd=self.ready();self.write_page(fd,bytes([255])*PAGE)               # all black
        self.screen(b'X');self.wait();self.write_page(fd,page_with(set()))
        self.assertEqual(self.screen(b'X'),{(x,y) for x in range(242,250) for y in range(8)})

if __name__=='__main__': unittest.main()
