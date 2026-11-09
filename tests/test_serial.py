# SPDX-License-Identifier: MIT
"""The OS serial channel (core/serial.c): wire framing, buffer overflow counting,
transport switching, and that OS data is only ever whole lines. Real code, no hardware."""
import ctypes as C, pathlib, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
class Counters(C.Structure): _fields_=[(n,C.c_uint32) for n in ('transport','tx_bytes','tx_dropped','rx_bytes','rx_dropped','tx_pending','rx_pending')]
def load():
    out=pathlib.Path(tempfile.mkdtemp())/'serial.so'
    subprocess.run(['cc','-shared','-fPIC','-O1','-Wall','-Wextra','-Werror','-I'+str(ROOT/'include'),str(ROOT/'core/serial.c'),'-o',str(out)],check=True)
    c=C.CDLL(str(out));c.serial_tx_line.restype=C.c_uint32;c.serial_write.restype=C.c_uint32;c.serial_rx_put.restype=C.c_uint32
    c.serial_transport_name.restype=C.c_char_p;return c
def decode(line):
    """What usb_kernel.py does with one wire line: (text, ended_with_newline)."""
    assert line.endswith('\n');body=line[:-1];whole=line.startswith('OS ');assert whole or line.startswith('OS~ ')
    body=body[3:] if whole else body[4:];out=bytearray();i=0
    while i<len(body):
        if body[i]=='\\':
            if body[i+1]=='\\':out.append(92);i+=2
            else:out.append(int(body[i+2:i+4],16));i+=4
        else:out.append(ord(body[i]));i+=1
    return bytes(out),whole
class Serial(unittest.TestCase):
    def setUp(self):
        self.c=load();self.assertEqual(self.c.serial_open(),0);self.c.serial_set_reader(True)
    def w(self,b):return self.c.serial_write(b,len(b))
    def lines(self):
        out=[];buf=C.create_string_buffer(272)
        while True:
            n=self.c.serial_tx_line(buf)
            if not n:return out
            out.append(buf.raw[:n].decode())
    def counters(self):
        k=Counters();self.c.serial_counters(C.byref(k));return k
    def test_lines_are_tagged_and_plain_text(self):
        self.w(b'hello\nworld');self.assertEqual(self.lines(),['OS hello\n','OS~ world\n'])
        self.w(b'!\n');self.assertEqual(self.lines(),['OS !\n'])
    def test_roundtrip_binary_through_escapes(self):
        data=bytes(range(256)).replace(b'\n',b'');self.w(data[:200])
        got=b''.join(decode(l)[0] for l in self.lines());self.assertEqual(got,data[:200])
    def test_every_line_is_printable_and_bounded(self):
        self.w(bytes(range(256))*1);ls=self.lines()
        for l in ls:
            self.assertLessEqual(len(l),271);self.assertEqual(l.count('\n'),1)
            self.assertTrue(all(32<=ord(ch)<127 for ch in l[:-1]))
    def test_never_inside_a_kernel_log_line(self):
        # The kernel appends each framed line whole to its log ring, as it does a LOG line.
        ring=[];self.w(b'a\nb\n')
        ring.append('LOG one\n');ring+=self.lines();ring.append('LOG two\n')
        self.assertEqual(ring,['LOG one\n','OS a\n','OS b\n','LOG two\n'])
        self.assertTrue(all(l.startswith(('LOG ','OS ')) and l.endswith('\n') for l in ring))
    def test_overflow_counts_and_never_stalls(self):
        n=self.w(b'x'*600);self.assertEqual(n,600);k=self.counters()
        self.assertEqual((k.tx_bytes,k.tx_dropped,k.tx_pending),(512,88,512))
    def test_no_reader_drops(self):
        self.c.serial_set_reader(False);self.assertEqual(self.w(b'abc'),3)
        k=self.counters();self.assertEqual((k.tx_bytes,k.tx_dropped,k.transport),(0,3,0));self.assertEqual(self.lines(),[])
    def test_transport_switching(self):
        self.assertEqual(self.c.serial_transport_name(),b'usb');self.w(b'queued')
        self.c.serial_set_reader(False);self.assertEqual(self.c.serial_transport_name(),b'none')
        k=self.counters();self.assertEqual((k.tx_pending,k.tx_dropped),(0,6))   # what waited is dropped, counted
        self.c.serial_set_reader(True);self.w(b'back\n');self.assertEqual(self.lines(),['OS back\n'])
    def test_rx_and_overflow(self):
        buf=C.create_string_buffer(300);self.assertEqual(self.c.serial_read(buf,10),-11)
        self.assertEqual(self.c.serial_rx_put(b'hi\n',3),3);self.assertEqual(self.c.serial_read(buf,2),2);self.assertEqual(buf.raw[:2],b'hi')
        self.assertEqual(self.c.serial_read(buf,10),1)
        self.assertEqual(self.c.serial_rx_put(b'z'*300,300),256);k=self.counters();self.assertEqual((k.rx_bytes,k.rx_dropped),(259,44))
    def test_one_owner_and_close_forgets(self):
        self.assertEqual(self.c.serial_open(),-16);self.c.serial_rx_put(b'q',1);self.c.serial_close()
        self.assertEqual(self.c.serial_rx_put(b'q',1),0);self.assertEqual(self.c.serial_open(),0)
        self.assertEqual(self.counters().rx_pending,0)
if __name__=='__main__': unittest.main()
