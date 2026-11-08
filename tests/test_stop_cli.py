# SPDX-License-Identifier: MIT
import pathlib
import subprocess
import sys
import unittest
from unittest.mock import patch

ROOT=pathlib.Path(__file__).resolve().parents[1]
sys.path.insert(0,str(ROOT/'scripts'))
import usb_kernel


class StopCLI(unittest.TestCase):
    def test_stop_is_exposed(self):
        p=subprocess.run([sys.executable,str(ROOT/'scripts/usb_kernel.py'),'--help'],capture_output=True,text=True)
        self.assertEqual(p.returncode,0,p.stderr)
        self.assertIn('stop',p.stdout)

    def test_stop_handshake_exact_reply_and_error(self):
        class Port:
            def __init__(self,reply): self.sent=[];self.lines=iter(['INFO 2 test', 'INFO 2 delayed',reply]);self.closed=False
            def send(self,data): self.sent.append(data)
            def reply(self,timeout): return next(self.lines)
            def close(self): self.closed=True
        for reply,status in [('OK stop',0),('OK stop idle',0),('ERR stop cleanup',1),('OK other',1)]:
            port=Port(reply)
            with patch.object(sys,'argv',['usb_kernel.py','fake','stop']),patch.object(usb_kernel,'Serial',return_value=port):
                self.assertEqual(usb_kernel.main(),status)
            self.assertEqual(port.sent,[b'INFO\n',b'STOP\n'])
            self.assertTrue(port.closed)
