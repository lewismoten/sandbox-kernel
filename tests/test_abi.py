# SPDX-License-Identifier: MIT
"""ABI layout (acceptance test 4, layout part): SDK and kernel records agree
on the host and on ARM32, the SDK wrappers emit svc, and abi_check enforces
the header rules."""
import ctypes as C
import os, pathlib, shutil, struct, subprocess, tempfile, unittest
ROOT=pathlib.Path(__file__).resolve().parents[1]
ARM=os.environ.get('TOOLCHAIN','/Applications/ArmGNUToolchain/15.3.rel2/arm-none-eabi/bin/arm-none-eabi-')+'gcc'
INC=['-I'+str(ROOT/'include'),'-I'+str(ROOT/'sdk/include')]
EXPECTED={'header':16,'open':56,'io':48,'close':24,'ioctl':48,'exit':24,'fb_mode':56,'fb_present':48}

class Layout(unittest.TestCase):
    def test_host_layout(self):
        subprocess.run(['cc','-std=c11','-fsyntax-only','-Wall','-Werror',*INC,str(ROOT/'tests/abi_layout.c')],check=True)
    @unittest.skipUnless(shutil.which(ARM) or os.path.exists(ARM),'Arm GNU toolchain not installed')
    def test_arm32_layout_and_svc(self):
        with tempfile.TemporaryDirectory() as d:
            o=pathlib.Path(d)/'abi.o'
            subprocess.run([ARM,'-std=c11','-mcpu=arm1176jzf-s','-marm','-mfloat-abi=soft','-Os','-ffreestanding','-Wall','-Wextra','-Werror','-c',*INC,str(ROOT/'tests/abi_layout.c'),'-o',str(o)],check=True)
            dis=subprocess.run([ARM.replace('gcc','objdump'),'-d',str(o)],check=True,capture_output=True,text=True).stdout
            self.assertGreaterEqual(dis.count('svc'),6)
    def test_sdk_header_alone(self):
        with tempfile.TemporaryDirectory() as d:
            src=pathlib.Path(d)/'only.c';src.write_text('#include <sandbox/abi.h>\nint x=sizeof(sbx_open_rec);\n')
            subprocess.run(['cc','-std=c11','-fsyntax-only','-Wall','-Werror','-I'+str(ROOT/'sdk/include'),str(src)],check=True)

class Check(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory();lib=pathlib.Path(cls.temp.name)/'abi.dylib'
        subprocess.run(['cc','-shared','-fPIC','-O1','-DHOST_TEST','-Wall','-Werror','-I'+str(ROOT/'tests/stubs'),*INC,str(ROOT/'core/abi.c'),'-o',str(lib)],check=True)
        cls.c=C.CDLL(str(lib));cls.c.abi_check.argtypes=[C.c_void_p,C.c_uint32,C.c_uint32,C.c_uint32,C.c_uint32]
    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()
    def check(self,raw,bytes_=None,major=1,minimum=48,known=48):
        buf=(C.c_uint64*((len(raw)+7)//8))();C.memmove(buf,raw,len(raw))
        return self.c.abi_check(buf,len(raw) if bytes_ is None else bytes_,major,minimum,known)
    def rec(self,size=48,flags=0,r0=0,r1=0,tail=b''):
        return struct.pack('<IIII',size,flags,r0,r1)+bytes(32)+tail
    def test_rules(self):
        self.assertEqual(self.check(self.rec()),0)
        self.assertEqual(self.check(self.rec(),major=2),-93)
        self.assertEqual(self.check(self.rec(),bytes_=40),-22)       # register size disagrees
        self.assertEqual(self.check(self.rec(size=40)[:40]),-22)    # below the minimum
        self.assertEqual(self.check(self.rec(flags=1)),-22)
        self.assertEqual(self.check(self.rec(r0=1)),-22)
        self.assertEqual(self.check(self.rec(r1=1)),-22)
        self.assertEqual(self.check(self.rec(size=56,tail=bytes(8))),0)   # newer minor, zero tail
        self.assertEqual(self.check(self.rec(size=56,tail=b'\1'+bytes(7))),-22)
        self.assertEqual(self.c.abi_check(None,48,1,48,48),-14)

class Advice(unittest.TestCase):
    def test_public_minor_advice_matches_abi_history(self):
        import re
        hist={int(m.group(1)) for m in re.finditer(r'^\| 1\.(\d+) \|',(ROOT/'docs/os-authors.md').read_text(),re.M)}
        top=int(re.search(r'K_ABI_MINOR (\d+)u',(ROOT/'include/abi.h').read_text()).group(1))
        for f in (ROOT/'sdk/include').rglob('*.h'):
            t=f.read_text()
            for m in re.finditer(r'minor\s*>=\s*(\d+)',t):
                n=int(m.group(1))
                self.assertLessEqual(n,top,f.name)
                self.assertIn(n,hist,'%s: minor >= %d is not in the ABI history table'%(f.name,n))
                tag=re.findall(r'/\*\s*1\.(\d+):',t[:m.start()])
                if tag: self.assertEqual(n,int(tag[-1]),'%s: advice minor >= %d disagrees with its 1.%s comment'%(f.name,n,tag[-1]))
