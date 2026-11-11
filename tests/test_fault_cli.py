# SPDX-License-Identifier: MIT
"""Fault/log helpers use checked GET data and optional local symbol tools."""
import contextlib
import io
import os
import pathlib
import sys
import tempfile
import unittest
import zlib
from unittest import mock

sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'scripts'))
import usb_kernel as usb

SAMPLE=('OS fault /OS/GMOS.ELF data abort pc 0x00102030 fsr 0x0000000d '
        'addr 0x12345678 up 1234 ms calls 56 presents 7 starts 2\n'
        '> loading game\n> frame ready\n')

class Port:
    def __init__(self,files,crc_bad=False):
        self.files=files;self.sent=[];self.crc_bad=crc_bad
    def send(self,command):
        self.sent.append(command)
        verb,path,ident,offset=command.decode().split();offset=int(offset)
        assert verb=='GET'
        if path not in self.files:
            self.header='ERR get open';return
        data=self.files[path];self.body=data[offset:offset+256]
        crc=zlib.crc32(self.body) ^ int(self.crc_bad)
        self.header=f'GET 1 {len(data)} {offset} {len(self.body)} {crc}'
    def line(self,timeout):return self.header
    def exact(self,n):return self.body

class FaultCLI(unittest.TestCase):
    def setUp(self):
        self.enterContext(mock.patch.object(usb,'handshake',return_value='INFO 2 test'))
        self.enterContext(mock.patch.object(usb.time,'sleep'))
        self.stdout=self.enterContext(contextlib.redirect_stdout(io.StringIO()))
        self.stderr=self.enterContext(contextlib.redirect_stderr(io.StringIO()))
    def test_sample_and_absent_lr(self):
        port=Port({usb.KERNEL_LOG_DIR+'FAULT.TXT':SAMPLE.encode()})
        self.assertEqual(usb.read_fault(port),0)
        text=self.stdout.getvalue()
        for value in ('Program: /OS/GMOS.ELF','Kind: data abort','PC: 0x00102030',
                      'LR: unavailable','FSR: 0x0000000d (permission fault (section))',
                      'Address: 0x12345678','Run: 1234 ms','Calls: 56','Presents: 7',
                      'Start count: 2','  loading game','  frame ready'):
            self.assertIn(value,text)
        self.assertEqual(port.sent[0],b'GET /OS/SAVES/KERNEL/FAULT.TXT 0 0\n')
    def test_missing(self):
        self.assertEqual(usb.read_fault(Port({})),0)
        self.assertEqual(self.stdout.getvalue(),'no fault recorded\n')
    def test_corrupt_and_malformed_are_errors(self):
        path=usb.KERNEL_LOG_DIR+'FAULT.TXT'
        with self.assertRaisesRegex(ValueError,'CRC'):
            usb.read_fault(Port({path:SAMPLE.encode()},crc_bad=True))
        for data in (b'',b'garbage',b'OS fault /OS/A.ELF data abort pc nope'):
            with self.assertRaisesRegex(ValueError,'malformed'):usb.read_fault(Port({path:data}))
        self.assertNotIn('no fault recorded',self.stdout.getvalue())
        with mock.patch.object(usb,'card_reply',return_value='ERR get read'):
            with self.assertRaisesRegex(ValueError,'get reply'):usb.read_fault(Port({}))
    def test_fsr_status_bits(self):
        for status,word in ((1,'alignment'),(5,'translation'),(7,'page'),(9,'domain'),
                            (13,'permission'),(15,'page'),(0x406,'asynchronous'),(0x40f,'unknown')):
            self.assertIn(word,usb.decode_fsr(status))
        self.assertEqual(usb.decode_fsr(0x80d),usb.decode_fsr(13))
    def test_non_abort_kind(self):
        for kind in ('exit -1','missed heartbeat','undefined instruction'):
            record=usb.parse_fault(SAMPLE.replace('data abort',kind))
            self.assertEqual(record['kind'],kind)
    def test_no_toolchain(self):
        with mock.patch.object(usb.shutil,'which',return_value=None):
            usb.fault_symbols(usb.parse_fault(SAMPLE),'GMOS.ELF')
        self.assertIn('no arm-none-eabi-addr2line',self.stdout.getvalue())
    def test_fake_addr2line_pc_and_lr(self):
        sample=SAMPLE.replace(' fsr ',' lr 0x00104050 fsr ')
        with tempfile.TemporaryDirectory() as directory:
            folder=pathlib.Path(directory);elf=folder/'GMOS with spaces.ELF';elf.touch()
            tool=folder/'arm-none-eabi-addr2line'
            tool.write_text('#!'+sys.executable+'\nimport sys\n'
                            'assert sys.argv[1:4] == ["-f", "-C", "-e"]\n'
                            'assert sys.argv[4].endswith("GMOS with spaces.ELF")\n'
                            'print("tick" if sys.argv[5]=="0x102030" else "caller")\n'
                            'print("game.c:42")\n')
            tool.chmod(0o755)
            with mock.patch.dict(os.environ,{'PATH':directory}):
                self.assertEqual(usb.read_fault(Port({usb.KERNEL_LOG_DIR+'FAULT.TXT':sample.encode()}),elf),0)
        self.assertIn('PC symbol: tick:game.c:42',self.stdout.getvalue())
        self.assertIn('LR symbol: caller:game.c:42',self.stdout.getvalue())
    def test_llvm_fallback_and_tool_failure(self):
        import subprocess
        record=usb.parse_fault(SAMPLE)
        for name in ('llvm-addr2line','llvm-symbolizer'):
            with mock.patch.object(usb.shutil,'which',side_effect=lambda n: '/bin/'+n if n==name else None), \
                 mock.patch.object(pathlib.Path,'is_file',return_value=True), \
                 mock.patch.object(usb.subprocess,'run',return_value=subprocess.CompletedProcess([],0,'tick\ngame.c:4\n')) as run:
                usb.fault_symbols(record,'GMOS.ELF')
                self.assertEqual(run.call_args.args[0][-1],'0x102030')
                self.assertIn('--obj=GMOS.ELF' if name=='llvm-symbolizer' else '-e',run.call_args.args[0])
        with mock.patch.object(usb.shutil,'which',return_value='/bin/llvm-addr2line'), \
             mock.patch.object(pathlib.Path,'is_file',return_value=True), \
             mock.patch.object(usb.subprocess,'run',side_effect=subprocess.TimeoutExpired('addr2line',10)):
            usb.fault_symbols(record,'GMOS.ELF')
        self.assertIn('symbol resolution failed',self.stderr.getvalue())
    def test_logs_bytes_multiple_chunks_and_missing(self):
        files={usb.KERNEL_LOG_DIR+n:bytes(range(256))*2+b'\r\n' for n in ('BOOT.TXT','USB.TXT','AUDIO.TXT','FAULT.TXT')}
        port=Port(files)
        with tempfile.TemporaryDirectory() as directory:
            folder=pathlib.Path(directory)/'logs'
            self.assertEqual(usb.collect_logs(port,folder),0)
            self.assertEqual(sorted(p.name for p in folder.iterdir()),['AUDIO.TXT','BOOT.TXT','FAULT.TXT','USB.TXT'])
            for p in folder.iterdir():self.assertEqual(p.read_bytes(),files[usb.KERNEL_LOG_DIR+p.name])
        self.assertIn('CEC.TXT: unavailable',self.stderr.getvalue())
        self.assertEqual(len(port.sent),13)
    def test_logs_crc_does_not_publish_bad_file(self):
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(ValueError,'CRC'):
                usb.collect_logs(Port({usb.KERNEL_LOG_DIR+'BOOT.TXT':b'bad'},crc_bad=True),directory)
            self.assertEqual(list(pathlib.Path(directory).iterdir()),[])
    def test_default_log_folder(self):
        with tempfile.TemporaryDirectory() as directory:
            folder=pathlib.Path(directory)/'kernel-logs-20261110-123456'
            with mock.patch.object(usb.time,'strftime',return_value=str(folder)):
                self.assertEqual(usb.collect_logs(Port({usb.KERNEL_LOG_DIR+'BOOT.TXT':b'boot'})),0)
            self.assertEqual((folder/'BOOT.TXT').read_bytes(),b'boot')
    def test_cli_dispatch_and_close(self):
        for args,handler in ((['fault','--elf','GMOS.ELF'],'read_fault'),(['logs','--out','saved'],'collect_logs')):
            with mock.patch.object(sys,'argv',['usb_kernel.py','fake-port',*args]), \
                 mock.patch.object(usb,'Serial') as serial, mock.patch.object(usb,handler,return_value=0) as run:
                self.assertEqual(usb.main(),0)
                run.assert_called_once_with(serial.return_value,pathlib.Path(args[-1]))
                serial.return_value.close.assert_called_once()

if __name__=='__main__':unittest.main()
