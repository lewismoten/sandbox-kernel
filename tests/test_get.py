# SPDX-License-Identifier: MIT
"""Read-only card retrieval through the real parser/FAT and the POSIX client."""
import ctypes as C
import contextlib
import io
import os
import pathlib
import tempfile
import struct
import unittest
import zlib
from unittest import mock
import test_m1_user as u
import usb_kernel as usb

class Wire(u.ProtocolBase):
    def setUp(self):
        super().setUp();self.c.take_output.restype=C.c_void_p
        self.data=bytes(range(256))*2+b'tail'
        self.assertEqual(self.c.fatput(b'TEST.TXT',self.data,len(self.data)),0)
    def command(self,text,advance=2000,card=True):
        self.c.advance(advance);b=(text+'\n').encode();self.c.feed(b,len(b));self.c.protocol_task()
        if card:self.c.protocol_card_task()
        n=self.c.output_size();raw=C.string_at(self.c.take_output(),n);self.c.protocol_task()
        return raw
    def start(self):self.assertTrue(self.command('INFO').startswith(b'INFO 2 '))
    def chunk(self,raw,offset,previous=None):
        line,body=raw.split(b'\n',1);meta,n,crc=usb.get_header(line.decode(),offset,previous)
        self.assertEqual(len(body),n);self.assertEqual(zlib.crc32(body),crc);return meta,body
    def test_chunks_crc_rate_stale_reset_and_no_writes(self):
        self.assertEqual(self.command('GET /OS/TEST.TXT 0 0'),b'ERR not-ready\n');self.start()
        writes=self.c.write_count();meta,body=self.chunk(self.command('GET /OS/TEST.TXT 0 0'),0)
        self.assertEqual(body,self.data[:256])
        self.assertEqual(self.command(f'GET /OS/TEST.TXT {meta[0]} 256',0),b'ERR get busy\n')
        _,body=self.chunk(self.command(f'GET /OS/TEST.TXT {meta[0]} 256'),256,meta);self.assertEqual(body,self.data[256:512])
        _,body=self.chunk(self.command(f'GET /OS/TEST.TXT {meta[0]} 512'),512,meta);self.assertEqual(body,b'tail')
        self.assertEqual(self.command(f'GET /OS/TEST.TXT {meta[0]} 0'),b'ERR get stale\n')
        self.command('GET /OS/TEST.TXT 0 0');self.c.protocol_reset()
        self.assertEqual(self.c.asset_cache_readers(),0)
        self.assertEqual(self.c.write_count(),writes)
    def test_paths_ranges_and_types(self):
        self.start()
        for path in ('relative','/../OS/TEST.TXT','/OS/./TEST.TXT','//OS/TEST.TXT','/OS//TEST.TXT','/OS/TEST.TXT/','/dev/../OS/TEST.TXT','/OS\\TEST.TXT','/'+('a'*192),'/'+'/'.join(['a']*17)):
            self.assertEqual(self.command(f'GET {path} 0 0'),b'ERR get command\n',path)
        for args in ('0 1','0 4294967296','0 -1','0 0 extra'):
            self.assertTrue(self.command('GET /OS/TEST.TXT '+args).startswith(b'ERR get '))
        self.assertEqual(self.command('GET /MISSING.TXT 0 0'),b'ERR get open\n')
        self.assertEqual(self.command('GET /OS 0 0'),b'ERR get size-or-type\n')
        self.assertEqual(self.command('LS /OS/TEST.TXT 0 0'),b'ERR get read\n')
    def test_foreground_defer_timeout_and_running_os(self):
        self.start()
        self.assertEqual(self.command('GET /OS/TEST.TXT 0 0',card=False),b'')
        self.c.protocol_card_task();n=self.c.output_size();meta,_=self.chunk(C.string_at(self.c.take_output(),n),0);self.c.protocol_task()
        self.assertEqual(self.command(f'GET /OS/TEST.TXT {meta[0]} 256',2000001),b'ERR get stale\n')
        self.c.take_output.restype=C.c_char_p;self.assertIn('OK file',self.put(u.program()))
        self.c.fb_setup(0,1);self.assertEqual(self.c.os_start(b'/OS/DIAG.ELF'),0)
        self.c.take_output.restype=C.c_void_p;self.start()
        self.chunk(self.command('GET /OS/TEST.TXT 0 0'),0);self.assertTrue(self.c.os_running())
    def test_fault_record_reader_lock_release(self):
        self.c.fatsave_put.argtypes=[C.c_char_p,C.c_char_p,C.c_char_p,C.c_uint32]
        data=b'crash record\n'+b'x'*300
        self.assertEqual(self.c.fatsave_put(b'KERNEL',b'FAULT.TXT',data,len(data)),0)
        self.start();before=self.c.write_count()
        meta,first=self.chunk(self.command('GET /OS/SAVES/KERNEL/FAULT.TXT 0 0'),0)
        self.assertEqual(self.c.asset_cache_readers(),1)
        self.assertEqual(self.c.fatsave_put(b'KERNEL',b'FAULT.TXT',b'new',3),-16)
        _,last=self.chunk(self.command(f'GET /OS/SAVES/KERNEL/FAULT.TXT {meta[0]} 256'),256,meta)
        self.assertEqual(first+last,data);self.assertEqual(self.c.asset_cache_readers(),0)
        self.assertEqual(self.c.write_count(),before)
        self.command('GET /OS/SAVES/KERNEL/FAULT.TXT 0 0')
        self.assertEqual(self.c.asset_cache_readers(),1)
        self.c.protocol_reset();self.assertEqual(self.c.asset_cache_readers(),0)
    def test_empty_and_size_cap(self):
        self.c.close_disk();disk,_=u.boot_card(self.work,{'EMPTY.TXT':b'x','BIG.BIN':bytes(usb.GET_MAX+1)})
        # The boot-image helper skips empty inputs; turn this fixture's entry
        # into an empty file (a retained allocation is legal FAT).
        with disk.open('r+b') as f:
            f.seek(6176*512);root=bytearray(f.read(1024))
            at=root.index(b'EMPTY   TXT');struct.pack_into('<I',root,at+28,0)
            f.seek(6176*512);f.write(root)
        self.c.open_disk(os.fsencode(disk));self.assertEqual(self.c.asset_mount(),0);self.start()
        meta,body=self.chunk(self.command('GET /EMPTY.TXT 0 0'),0);self.assertEqual((meta[1],body),(0,b''))
        self.assertEqual(self.command('GET /BIG.BIN 0 0'),b'ERR get size-or-type\n')
    def test_tool_and_ls_end_to_end(self):
        owner=self
        class Port:
            def send(self,command):self.buf=bytearray(owner.command(command.decode().rstrip('\n')))
            def line(self,timeout):
                raw,_,self.buf=self.buf.partition(b'\n');return raw.decode()
            def exact(self,n):raw=bytes(self.buf[:n]);del self.buf[:n];return raw
        self.start()
        with tempfile.TemporaryDirectory() as d,mock.patch.object(usb,'handshake',return_value='INFO 2'),mock.patch.object(usb.time,'sleep'):
            out=pathlib.Path(d)/'out';self.assertEqual(usb.get_file(Port(),'/OS/TEST.TXT',out),0);self.assertEqual(out.read_bytes(),self.data)
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                self.assertEqual(usb.list_card(Port(),'/OS'),0)
            self.assertIn('TEST.TXT',stdout.getvalue())

class Host(unittest.TestCase):
    def test_validation(self):
        for p in ('/','/OS/SAVES/KERNEL/FAULT.TXT','/a-b.txt'):self.assertEqual(usb.card_path(p),p)
        for p in ('../x','/../x','/a/','//a','/a b','/a\\b','/é','/a\n','/'+('a'*192)):
            with self.assertRaises(ValueError):usb.card_path(p)
        good='GET 1 3 0 3 123';self.assertEqual(usb.get_header(good,0),((1,3),3,123))
        for line in (good+' x','GET 0 3 0 3 123','GET 1 3 1 3 123','GET 1 3 0 4 123','GET 1 4194305 0 256 1','GET 1 3 0 3 4294967296'):
            with self.assertRaises(ValueError):usb.get_header(line,0)
        with self.assertRaises(ValueError):usb.get_header(good,0,(2,3))
    def test_stdout_crc_and_noise(self):
        class Port:
            def __init__(self,data,crc=None):self.data=data;self.crc=zlib.crc32(data) if crc is None else crc
            def send(self,command):self.lines=iter(['LOG test','OS hello','OS~ hi','INFO 2 delayed','ERR get busy'] if not hasattr(self,'tried') else [f'GET 1 {len(self.data)} 0 {len(self.data)} {self.crc}']);self.tried=True
            def line(self,timeout):return next(self.lines)
            def exact(self,n):return self.data
        with mock.patch.object(usb,'handshake',return_value='INFO 2'),mock.patch.object(usb.time,'sleep'):
            with contextlib.redirect_stdout(io.StringIO()) as out:usb.get_file(Port(b'fault\n'),'/FAULT.TXT')
            self.assertEqual(out.getvalue(),'fault\n')
            with tempfile.TemporaryDirectory() as d:
                out=pathlib.Path(d)/'out'
                with self.assertRaisesRegex(ValueError,'CRC'):usb.get_file(Port(b'bad',0),'/X',out)
                self.assertFalse(out.exists())
            with self.assertRaises(ValueError):usb.get_file(Port(b'\0'),'/X')

if __name__=='__main__':unittest.main()
