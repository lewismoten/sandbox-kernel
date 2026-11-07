#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mac/POSIX stdlib CDC sender and log tail. Use /dev/cu.usbmodem* on macOS."""
import argparse, errno, hashlib, os, pathlib, re, select, sys, termios, time, tty, zlib
from image import validate

class Serial:
    def __init__(self,path):
        self.fd=os.open(path,os.O_RDWR|os.O_NOCTTY|os.O_NONBLOCK)
        self.original=termios.tcgetattr(self.fd)
        tty.setraw(self.fd)
        attrs=termios.tcgetattr(self.fd);attrs[2]|=termios.CLOCAL|termios.CREAD;termios.tcsetattr(self.fd,termios.TCSANOW,attrs)
        self.buf=bytearray()
    def close(self):
        try: termios.tcsetattr(self.fd,termios.TCSANOW,self.original)
        except termios.error: pass
        os.close(self.fd)
    def send(self,data,timeout=10):
        end=time.monotonic()+timeout
        while data:
            remaining=end-time.monotonic()
            if remaining<=0 or not select.select([],[self.fd],[],remaining)[1]: raise TimeoutError('serial write')
            try: n=os.write(self.fd,data)
            except BlockingIOError: continue
            if not n: raise OSError('disconnected')
            data=data[n:]
    def line(self,timeout):
        end=time.monotonic()+timeout
        while b'\n' not in self.buf:
            remaining=end-time.monotonic()
            if remaining<=0 or not select.select([self.fd],[],[],remaining)[0]: raise TimeoutError('serial reply')
            try: data=os.read(self.fd,4096)
            except BlockingIOError: continue
            if not data: raise OSError('CDC disconnected')
            self.buf.extend(data)
            if len(self.buf)>65536: raise ValueError('unbounded device line')
        raw,_,tail=self.buf.partition(b'\n');self.buf=bytearray(tail)
        return raw.rstrip(b'\r').decode('ascii',errors='replace')
    def reply(self,timeout):
        end=time.monotonic()+timeout
        while True:
            line=self.line(max(0,end-time.monotonic()))
            if line.startswith('LOG '): print(line,file=sys.stderr);continue
            return line

def handshake(port,attempts=5):
    for attempt in range(attempts):
        port.send(b'INFO\n')
        try:
            end=time.monotonic()+3
            while True:
                line=port.reply(max(0,end-time.monotonic()))
                if line.startswith('INFO 2 '): return line
        except TimeoutError:
            if attempt+1==attempts: raise
            print(f'INFO retry {attempt+1}',file=sys.stderr)
    raise TimeoutError('INFO')

def expect(port,wanted,timeout):
    line=port.reply(timeout)
    # A delayed reply to a retried INFO can precede READY.
    while line.startswith('INFO 2 '): line=port.reply(timeout)
    if line!=wanted: raise ValueError(f'Expected {wanted!r}, got {line!r}')

def send_kernel(port,path):
    image=validate(path.read_bytes());info=handshake(port);print(info)
    if 'features=kernel,no-os' not in info: raise ValueError('Device updates locked; inspect logs')
    before=dict(item.split('=',1) for item in info.split() if '=' in item)
    port.send(f'PUT2 kernel zero-w-armv6 {len(image)} {zlib.crc32(image):08x}\n'.encode())
    expect(port,'READY 1024',5)
    for offset in range(0,len(image),1024):
        block=image[offset:offset+1024];port.send(block)
        final=offset+len(block)==len(image)
        expect(port,'VERIFY' if final else f'ACK {offset+len(block)}',10)
    # Never repeat PUT2 after VERIFY. A lost reply must be resolved by INFO.
    try: expect(port,'OK pending rebooting',120)
    except (OSError,TimeoutError) as exc:
        print(f'Final result unknown ({exc}); reconnect and run info. Prior confirmed={before.get("confirmed")}. Do not resend blindly.',file=sys.stderr)
        return 2
    print('Pending committed; reconnect, run info, and watch for 30-second confirmation.')
    return 0

NAME83=r'[A-Z0-9_-]{1,8}(\.[A-Z0-9_-]{1,3})?'
def send_file(port,path,name):
    """PUTF: install one development file as /OS/<name>, or as /GAMES/<DIR>/<NAME>
    when name is GAMES/<DIR>/<NAME> (8.3, upper case; DIR without an extension)."""
    data=path.read_bytes()
    if not (re.fullmatch(NAME83,name) or re.fullmatch(r'GAMES/[A-Z0-9_-]{1,8}/'+NAME83,name)): raise ValueError(f'{name!r} is not an upper-case 8.3 name or GAMES/<DIR>/<NAME>')
    if not 0<len(data)<=1024*1024: raise ValueError('file must be 1 byte to 1 MiB')
    print(handshake(port))
    port.send(f'PUTF {name} {len(data)} {zlib.crc32(data):08x} {hashlib.sha256(data).hexdigest()}\n'.encode())
    expect(port,'READY 1024',5)
    for offset in range(0,len(data),1024):
        block=data[offset:offset+1024];port.send(block)
        expect(port,'VERIFY' if offset+len(block)==len(data) else f'ACK {offset+len(block)}',10)
    shown=f'/{name}' if '/' in name else f'/OS/{name}'
    expect(port,f'OK file {shown} {len(data)}',120)
    print(f'Installed {shown} ({len(data)} bytes)');return 0

def send_folder(port,folder,dir_name,root='GAMES'):
    """Install every file of a game folder as /GAMES/<DIR>/<NAME>, one PUTF each."""
    files=sorted(p for p in folder.iterdir() if p.is_file() and not p.name.startswith('.'))
    for p in files: send_file(port,p,f'{root}/{dir_name}/{p.name.upper()}')
    return 0

# Synthetic input (KEY/AXIS commands): Linux key codes by name.
KEYS={'esc':1,'q':16,'enter':28,'space':57,'up':103,'left':105,'right':106,'down':108}
def send_keys(port,names,gap,seconds):
    """Press and release each key in turn (or KEY <code> <down|up> with name:down / name:up)."""
    print(handshake(port))
    for item in names:
        name,_,how=item.partition(':');code=KEYS.get(name.lower()) or int(name)
        for state in ([how] if how else ['down','up']):
            port.send(f'KEY {code} {state}\n'.encode());line=port.reply(5)
            while line.startswith('INFO 2 '): line=port.reply(5)
            print(line,flush=True)
            if not line.startswith('OK key'): return 1
            time.sleep(gap)
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        try: print(port.line(max(0,end-time.monotonic())),flush=True)
        except TimeoutError: break
    return 0

def send_command(port,command):
    """One line command (AXIS <code> <value>, INPUT add|remove) and its reply."""
    print(handshake(port));port.send((command+'\n').encode());line=port.reply(5)
    while line.startswith('INFO 2 '): line=port.reply(5)
    print(line);return 0 if line.startswith('OK') else 1

def run_program(port,name,seconds):
    """RUN: start /OS/<name>, then show the log for a while."""
    print(handshake(port));port.send(f'RUN {name}\n'.encode())
    line=port.reply(10)
    while line.startswith('INFO 2 '): line=port.reply(10)
    print(line,flush=True)
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        try: print(port.line(max(0,end-time.monotonic())),flush=True)
        except TimeoutError: break
    return 0 if line.startswith('OK run') else 1

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('port');sub=ap.add_subparsers(dest='action',required=True)
    sub.add_parser('info');sub.add_parser('tail');p=sub.add_parser('send');p.add_argument('kernel',type=pathlib.Path)
    p=sub.add_parser('put',help='install a file as /OS/<name>');p.add_argument('file',type=pathlib.Path);p.add_argument('--name')
    p=sub.add_parser('run',help='start /OS/<name> and show the log');p.add_argument('name');p.add_argument('--seconds',type=float,default=15)
    p=sub.add_parser('putgame',help='install a game folder as /GAMES/<DIR>/');p.add_argument('folder',type=pathlib.Path);p.add_argument('dir')
    p=sub.add_parser('putapp',help='install an app folder as /OS/APPS/<DIR>/');p.add_argument('folder',type=pathlib.Path);p.add_argument('dir')
    p=sub.add_parser('key',help='press keys on the synthetic /dev/input/event0: up down enter esc q, a code, or name:down / name:up')
    p.add_argument('keys',nargs='+');p.add_argument('--gap',type=float,default=0.3);p.add_argument('--seconds',type=float,default=2)
    p=sub.add_parser('axis',help='AXIS <code> <value> on the synthetic device');p.add_argument('code',type=int);p.add_argument('value',type=int)
    p=sub.add_parser('input',help='add or remove the synthetic device');p.add_argument('state',choices=('add','remove'))
    a=ap.parse_args();port=Serial(a.port)
    try:
        if a.action=='send': return send_kernel(port,a.kernel)
        if a.action=='put': return send_file(port,a.file,a.name or a.file.name.upper())
        if a.action=='run': return run_program(port,a.name.upper(),a.seconds)
        if a.action=='putgame': return send_folder(port,a.folder,a.dir.upper())
        if a.action=='putapp': return send_folder(port,a.folder,a.dir.upper(),'APPS')
        if a.action=='key': return send_keys(port,a.keys,a.gap,a.seconds)
        if a.action=='axis': return send_command(port,f'AXIS {a.code} {a.value}')
        if a.action=='input': return send_command(port,f'INPUT {a.state}')
        print(handshake(port),flush=True)
        if a.action=='tail':
            while True:
                try: print(port.line(60),flush=True)
                except TimeoutError: pass
        return 0
    finally: port.close()
if __name__=='__main__':
    try: sys.exit(main())
    except KeyboardInterrupt: sys.exit(130)
    except (OSError,ValueError,TimeoutError) as exc: print(str(exc),file=sys.stderr);sys.exit(1)
