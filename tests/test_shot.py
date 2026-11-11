# SPDX-License-Identifier: MIT
"""Real USB parser/capture code over fake CDC; independent PNG decoding."""
import ctypes as C, os, pathlib, struct, sys, tempfile, unittest, zlib
from unittest import mock
import test_m1_user as u
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import usb_kernel as usb

class Wire(u.ProtocolBase):
    def setUp(self):
        super().setUp();self.c.take_output.restype=C.c_void_p
    def command(self,text,advance=2000):
        self.c.advance(advance);b=(text+'\n').encode();self.c.feed(b,len(b))
        self.c.protocol_task();n=self.c.output_size();raw=C.string_at(self.c.take_output(),n)
        self.c.protocol_task()  # finish binary drain
        return raw
    def start(self): self.assertTrue(self.command('INFO').startswith(b'INFO 2 '))
    def chunk(self,raw,kind,off,previous=None):
        line,body=raw.split(b'\n',1);meta,n,crc=usb.shot_header(line.decode(),kind,off,previous)
        self.assertEqual(len(body),n);self.assertEqual(zlib.crc32(body),crc);return meta,body
    def test_eink_binary_crc_snapshot_tail_and_stale(self):
        self.assertEqual(self.command('EINKSHOT 0 0'),b'ERR not-ready\n');self.start()
        meta,first=self.chunk(self.command('EINKSHOT 0 0'),'EINK',0)
        self.assertEqual(first,bytes(range(256)))  # contains NUL, newline and high bytes
        self.assertLessEqual(len(self.command(f'EINKSHOT {meta[0]} 256')),512)
        meta2,tail=self.chunk(self.command(f'EINKSHOT {meta[0]} 3840'),'EINK',3840,meta)
        self.assertEqual(len(tail),160)
        self.command('EINKSHOT 0 0')
        self.assertEqual(self.command(f'EINKSHOT {meta[0]} 256'),b'ERR shot stale\n')
        for args in ('0 1','0 4096','0 -1','0 4294967296','0 0 extra'):
            self.assertTrue(self.command('EINKSHOT '+args).startswith(b'ERR shot '))
    def test_rate_limit_and_reset(self):
        self.start();self.command('EINKSHOT 0 0')
        self.assertEqual(self.command('EINKSHOT 0 0',0),b'ERR shot busy\n')
        self.c.protocol_reset();self.assertEqual(self.command('EINKSHOT 0 0'),b'ERR not-ready\n')
    def test_fb_live_padded_rows_scale_mode_change_and_no_writes(self):
        self.start();p=self.c.fb_setup(0,1)
        source=bytes((i%251 for i in range(2700*480)));C.memmove(p,source,len(source))
        meta,body=self.chunk(self.command('FBSHOT 2 0 0'),'FB',0)
        self.assertEqual(meta[1:5],(320,240,1280,32))
        self.assertEqual(body,b''.join(source[i*8:i*8+4] for i in range(64)))
        _,body=self.chunk(self.command(f'FBSHOT 2 {meta[0]} 1280'),'FB',1280,meta)
        self.assertEqual(body,b''.join(source[5400+i*8:5400+i*8+4] for i in range(64)))
        self.assertEqual(C.string_at(p,len(source)),source)
        self.c.fb_set_mode(320,240,16)
        self.assertEqual(self.command(f'FBSHOT 2 {meta[0]} 256'),b'ERR shot stale\n')
        for scale in (0,17,4294967295):self.assertEqual(self.command(f'FBSHOT {scale} 0 0'),b'ERR shot range\n')
        self.c.fb_setup(0,0);self.assertEqual(self.command('FBSHOT 1 0 0'),b'ERR shot unavailable\n')
    def test_capture_while_os_runs(self):
        self.start();self.c.fb_setup(0,1)
        self.c.take_output.restype=C.c_char_p
        self.assertIn('OK file',self.put(u.program()))
        self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'),0);self.assertTrue(self.c.os_running())
        self.c.take_output.restype=C.c_void_p
        self.chunk(self.command('FBSHOT 1 0 0'),'FB',0)
        self.chunk(self.command('EINKSHOT 0 0'),'EINK',0)
        self.assertTrue(self.c.os_running())
    def test_capture_limits_before_reading_and_no_sticky_error(self):
        self.start();self.c.fb_setup(0,1)
        class Info(C.Structure):
            _fields_=[(n,C.c_uint32) for n in ('width','height','depth','pitch','bytes','order','bus','phys')]
        f=Info.in_dll(self.c,'fb0')
        f.width=f.height=4096;f.depth=32;f.pitch=4096*4;f.bytes=f.pitch*f.height
        before=C.c_char_p.in_dll(self.c,'last_error').value
        self.assertEqual(self.command('FBSHOT 1 0 0'),b'ERR shot range\n')
        f.width=4097
        self.assertEqual(self.command('FBSHOT 16 0 0'),b'ERR shot unavailable\n')
        self.assertEqual(C.c_char_p.in_dll(self.c,'last_error').value,before)
    def test_tool_end_to_end_binary_capture(self):
        owner=self
        class Port:
            def send(self,command): self.buf=bytearray(owner.command(command.decode().rstrip('\n')))
            def line(self,timeout):
                raw,_,tail=self.buf.partition(b'\n');self.buf=tail;return raw.decode()
            def exact(self,n): raw=bytes(self.buf[:n]);del self.buf[:n];return raw
        self.start()
        with tempfile.TemporaryDirectory() as d, mock.patch.object(usb,'handshake',return_value='INFO 2'), mock.patch.object(usb.time,'sleep'):
            out=pathlib.Path(d)/'panel.png'
            self.assertEqual(usb.screen_capture(Port(),out,True),0)
            _,w,h,stride,depth,order,total=(1,122,250,16,1,1,4000)
            plane=bytes(i%256 for i in range(4000))
            pw,ph,rgb=usb.shot_rgb((1,w,h,stride,depth,order,total),plane)
            self.assertEqual(out.read_bytes(),usb.rgb_png(pw,ph,rgb))
    def test_indexed_palette(self):
        self.start();p=self.c.fb_setup(0,1);self.c.fb_set_mode(16,16,8)
        meta,palette=self.chunk(self.command('FBSHOT 1 0 0'),'FB',0)
        self.assertEqual(meta[-1],1280)
        self.assertEqual(self.command(f'FBSHOT 1 {meta[0]} 1024'),b'ERR shot palette\n')
        color=(C.c_uint32*1)(0x123456);self.c.fb_set_palette(0,1,color)
        _,palette=self.chunk(self.command('FBSHOT 1 0 0'),'FB',0)
        self.assertEqual(palette[:4],b'\x56\x34\x12\x00')
        _,body=self.chunk(self.command(f'FBSHOT 1 {meta[0]} 1024'),'FB',1024)
        self.assertEqual(body,bytes(256))

class Host(unittest.TestCase):
    def test_png_chunks_crc_dimensions_scanlines(self):
        rgb=bytes((255,0,0,0,255,0,0,0,255,18,52,86));png=usb.rgb_png(2,2,rgb)
        self.assertEqual(png[:8],b'\x89PNG\r\n\x1a\n');pos=8;chunks={}
        while pos<len(png):
            n=struct.unpack_from('>I',png,pos)[0];tag=png[pos+4:pos+8];body=png[pos+8:pos+8+n]
            self.assertEqual(struct.unpack_from('>I',png,pos+8+n)[0],zlib.crc32(tag+body));chunks[tag]=body;pos+=n+12
        self.assertEqual(struct.unpack('>2I5B',chunks[b'IHDR']),(2,2,8,2,0,0,0))
        self.assertEqual(zlib.decompress(chunks[b'IDAT']),b'\0'+rgb[:6]+b'\0'+rgb[6:]);self.assertEqual(chunks[b'IEND'],b'')
        with self.assertRaises(ValueError):usb.rgb_png(2,2,b'')
    def test_native_colors(self):
        for order,data in ((0,b'\x33\x22\x11\0'),(1,b'\x11\x22\x33\0')):
            self.assertEqual(usb.shot_rgb((1,1,1,4,32,order,4),data),(1,1,b'\x11\x22\x33'))
        for order,val in ((0,0xf800),(1,0x001f)):
            self.assertEqual(usb.shot_rgb((1,1,1,2,16,order,2),struct.pack('<H',val)),(1,1,b'\xff\0\0'))
        palette=struct.pack('<I',0x123456)+bytes(1020)
        self.assertEqual(usb.shot_rgb((1,1,1,1,8,0,1025),palette+b'\0'),(1,1,b'\x12\x34\x56'))
    def test_panel_orientation_and_padding(self):
        for order in (0,1):
            plane=bytearray(b'\xff'*4000);nx,ny=(121,0) if order else (0,249)
            plane[ny*16+nx//8]&=~(128>>(nx%8))
            w,h,rgb=usb.shot_rgb((1,122,250,16,1,order,4000),plane)
            self.assertEqual((w,h),(250,122));self.assertEqual(rgb[:3],b'\0'*3);self.assertEqual(rgb[3:],b'\xff'*(len(rgb)-3))
    def test_bad_headers(self):
        good='SHOT FB 1 2 2 8 32 0 16 0 16 123'
        meta,_,_=usb.shot_header(good,'FB',0)
        for line in (good.replace('16 0 16','17 0 16'),good.replace('8 32','9 32'),good.replace('32 0','24 0'),good.replace('FB','EINK'),good+' extra',good.replace(' 123',' 4294967296')):
            with self.assertRaises(ValueError):usb.shot_header(line,'FB',0)
        with self.assertRaises(ValueError):usb.shot_header(good,'FB',256)
        with self.assertRaises(ValueError):usb.shot_header(good,'FB',0,(2,)+meta[1:])
    def test_exact_binary_preserves_following_lines(self):
        p=usb.Serial.__new__(usb.Serial);p.buf=bytearray(b'\0\n\xffLOG tail\n')
        self.assertEqual(p.exact(3),b'\0\n\xff');self.assertEqual(p.buf,b'LOG tail\n')
    def test_exact_truncated_body_disconnects(self):
        read_fd,write_fd=os.pipe()
        try:
            os.write(write_fd,b'partial');os.close(write_fd);write_fd=None
            p=usb.Serial.__new__(usb.Serial);p.fd=read_fd;p.buf=bytearray()
            with self.assertRaisesRegex(OSError,'disconnected'):p.exact(256)
        finally:
            os.close(read_fd)
            if write_fd is not None:os.close(write_fd)
    def test_client_crc_does_not_write_file(self):
        class Port:
            def send(self,data):pass
            def line(self,timeout):return 'SHOT EINK 1 122 250 16 1 1 4000 0 256 0'
            def exact(self,n):return b'x'*n
        with tempfile.TemporaryDirectory() as d, mock.patch.object(usb,'handshake',return_value='INFO 2'):
            out=pathlib.Path(d)/'out.png'
            with self.assertRaisesRegex(ValueError,'CRC'):usb.screen_capture(Port(),out,True)
            self.assertFalse(out.exists())

if __name__=='__main__':unittest.main()
