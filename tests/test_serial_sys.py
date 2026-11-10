# SPDX-License-Identifier: MIT
"""/dev/serial0 through the real system-call path, and the USB TX command and INFO line."""
import ctypes as C, struct, unittest
import test_m1_user as u
from test_m1_user import poke, peek
EAGAIN,EBUSY,EINVAL,EACCES,ENOTTY=11,16,22,13,25
NB=0x100
class SerialSys(u.CallsBase):
    def setUp(self):
        super().setUp();self.c.serial_set_reader(True)
    def test_open_rules(self):
        self.assertEqual(self.open('/dev/serial0',2)[0],-EINVAL)        # nonblocking only
        self.assertEqual(self.open('/dev/serial0',3|NB)[0],-EINVAL)
        r,fd=self.open('/dev/serial0',2|NB);self.assertEqual(r,0)
        self.assertEqual(self.open('/dev/serial0',2|NB)[0],-EBUSY)      # one owner
        self.assertEqual(self.close(fd),0);self.assertEqual(self.open('/dev/serial0',2|NB)[0],0)
    def test_write_read_and_query(self):
        r,fd=self.open('/dev/serial0',2|NB)
        self.assertEqual(self.io(2,fd,self.BUF,16)[0],-EAGAIN)
        poke(self.c,self.BUF,b'hi\n');self.assertEqual(self.io(3,fd,self.BUF,3),(0,3))
        self.assertEqual(self.c.serial_rx_put(b'ok',2),2)
        r,n=self.io(2,fd,self.BUF,16);self.assertEqual((r,n,peek(self.c,self.BUF,2)),(0,2,b'ok'))
        q=self.BUF+0x400;poke(self.c,q,self.hdr(72)+bytes(56));self.assertEqual(self.ioctl(fd,0x60000,q,72),0)
        v=struct.unpack_from('<7Q',peek(self.c,q,72),16);self.assertEqual(v,(1,3,0,2,0,3,0))
        poke(self.c,q,self.hdr(72)+struct.pack('<Q',1)+bytes(48));self.assertEqual(self.ioctl(fd,0x60000,q,72),-EINVAL)
        self.assertEqual(self.ioctl(fd,0x60001,q,72),-ENOTTY)
    def test_program_end_releases_it(self):
        self.assertEqual(self.open('/dev/serial0',2|NB)[0],0);self.c.sys_reset()
        self.assertEqual(self.open('/dev/serial0',2|NB)[0],0)

class SerialUsb(u.ProtocolBase):
    def line(self,s):
        b=(s+'\n').encode();self.c.feed(b,len(b));return self.run_protocol(5).strip()
    def test_tx_command_and_info(self):
        self.assertEqual(self.line('TX 3'),'ERR not-ready')
        info=self.line('INFO');self.assertTrue(info.endswith('serial=none'),info)
        self.c.serial_set_reader(True);self.assertTrue(self.line('INFO').endswith('serial=usb'))
        self.assertEqual(self.line('TX 0'),'ERR tx length');self.assertEqual(self.line('TX 65'),'ERR tx length')
        self.assertEqual(self.line('TX 3'),'')                       # waits for the bytes
        self.c.feed(b'a\nb',3);self.assertEqual(self.run_protocol(5).strip(),'OK tx 0')   # nobody owns it: counted dropped
        self.assertEqual(self.c.serial_open(),0)
        self.assertEqual(self.line('TX 3'),'')
        self.c.feed(b'a\nb',3);self.assertEqual(self.run_protocol(5).strip(),'OK tx 3')   # the newline is data, not a command end
        buf=C.create_string_buffer(8);self.assertEqual(self.c.serial_read(buf,8),3);self.assertEqual(buf.raw[:3],b'a\nb')
    def test_stalled_tx_times_out_and_resynchronizes(self):          # S1: TX 64 plus 1 byte, then silence
        self.c.serial_set_reader(True);self.assertEqual(self.c.serial_open(),0);self.line('INFO')
        self.assertEqual(self.line('TX 64'),'');self.c.feed(b'x',1);self.assertEqual(self.run_protocol(5).strip(),'')
        self.c.advance(1_900_000);self.assertEqual(self.line('STOP'),'')   # inside the window STOP is still body bytes
        self.c.advance(2_100_000)
        self.assertEqual(self.run_protocol(5).strip(),'ERR tx-timeout')
        self.assertTrue(self.line('INFO').startswith('INFO'))               # replied, no reset needed
        self.assertEqual(self.line('TX 2'),'');self.c.feed(b'ab',2);self.assertEqual(self.run_protocol(5).strip(),'OK tx 2')
    def test_steady_tx_body_is_not_timed_out(self):
        self.c.serial_set_reader(True);self.assertEqual(self.c.serial_open(),0);self.line('INFO')
        self.assertEqual(self.line('TX 3'),'')
        for ch in b'abc':
            self.c.advance(1_500_000);self.c.feed(bytes([ch]),1);out=self.run_protocol(5).strip()
        self.assertEqual(out,'OK tx 3')
if __name__=='__main__': unittest.main()
