#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Mac/POSIX stdlib CDC sender and log tail. Use /dev/cu.usbmodem* on macOS."""
import argparse, errno, hashlib, os, pathlib, re, select, struct, sys, termios, time, tty, zlib
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
    def exact(self,n,timeout=5):
        """Binary body: consume buffered bytes first, never interpret newline/LOG."""
        if not 0<=n<=256: raise ValueError('capture body length')
        end=time.monotonic()+timeout
        while len(self.buf)<n:
            remaining=end-time.monotonic()
            if remaining<=0 or not select.select([self.fd],[],[],remaining)[0]: raise TimeoutError('capture body')
            try: data=os.read(self.fd,4096)
            except BlockingIOError: continue
            if not data: raise OSError('CDC disconnected')
            self.buf.extend(data)
        data=bytes(self.buf[:n]);del self.buf[:n];return data
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

def info_fields(info):
    """Parse INFO into exact key=value fields. A repeated key is kept as a list so callers can reject it."""
    out={}
    for item in info.split()[2:]:
        if '=' in item:
            k,v=item.split('=',1);out.setdefault(k,[]).append(v)
    return {k:(v[0] if len(v)==1 else v) for k,v in out.items()}

def check_updates(f):
    """Fail closed: exactly one updates= field saying ready, or (legacy kernels, no updates= field)
    exactly one features=kernel,no-os. features= is legacy; features=locked contradicts ready."""
    for k in ('updates','features','pending'):
        if isinstance(f.get(k),list): raise ValueError(f'Device INFO repeats {k}=; refusing to update')
    if 'pending' in f:  # a kernel that reports pending= must say 0 (decimal) before an update starts
        if not f['pending'].isascii() or not f['pending'].isdigit(): raise ValueError('Device INFO has a malformed pending=; refusing to update')
        if int(f['pending'])!=0: raise ValueError('Device has a pending update; wait for confirmation')
    if 'updates' in f:
        if f['updates']=='busy': raise ValueError('Device has a pending update; wait for confirmation')
        if f['updates']!='ready' or f.get('features')=='locked': raise ValueError('Device updates locked; inspect logs')
    elif f.get('features')!='kernel,no-os': raise ValueError('Device updates locked; inspect logs')

def send_kernel(port,path,companion=False):
    image=validate(path.read_bytes());info=handshake(port);print(info)
    before=info_fields(info);check_updates(before)
    if companion:
        if 'host_companion' not in before: raise ValueError('Update the device kernel first; it does not support host companions')
        if int.from_bytes(image[28:32],'little')!=1: raise ValueError('Host companion requires a USB_ROLE=host image')
    kind='host-companion' if companion else 'kernel'
    port.send(f'PUT2 {kind} zero-w-armv6 {len(image)} {zlib.crc32(image):08x}\n'.encode())
    expect(port,'READY 1024',5)
    for offset in range(0,len(image),1024):
        block=image[offset:offset+1024];port.send(block)
        final=offset+len(block)==len(image)
        expect(port,'VERIFY' if final else f'ACK {offset+len(block)}',10)
    # Never repeat PUT2 after VERIFY. A lost reply must be resolved by INFO.
    try: expect(port,'OK host companion installed' if companion else 'OK pending rebooting',120)
    except (OSError,TimeoutError) as exc:
        print(f'Final result unknown ({exc}); reconnect and run info. Prior confirmed={before.get("confirmed")}. Do not resend blindly.',file=sys.stderr)
        return 2
    print('Host companion installed. Power off, attach the OTG keyboard adapter, then power on.' if companion else 'Pending committed; reconnect, run info, and watch for 30-second confirmation.')
    return 0

NAME83=r'[A-Z0-9_-]{1,8}(\.[A-Z0-9_-]{1,3})?'
def send_file(port,path,name):
    """PUTF: install one development file as /OS/<name>, or as /GAMES/<DIR>/<NAME>
    when name is GAMES/<DIR>/<NAME> (8.3, upper case; DIR without an extension), or a
    packed game as GAMES/<NAME>.GMP or POCKET/<NAME>.GMP (further volumes .G01 to .G99),
    or the library index as GAMES/LIBRARY.JSL."""
    data=path.read_bytes()
    pack=re.fullmatch(r'(GAMES|POCKET)/[A-Z0-9_-]{1,8}\.G(MP|(?!00)[0-9]{2})',name) or name=='GAMES/LIBRARY.JSL'
    if not (re.fullmatch(NAME83,name) or re.fullmatch(r'(GAMES|APPS)/[A-Z0-9_-]{1,8}/'+NAME83,name) or pack): raise ValueError(f'{name!r} is not an upper-case 8.3 name, GAMES/<DIR>/<NAME>, APPS/<DIR>/<NAME> or a packed game (GAMES/<NAME>.GMP, POCKET/<NAME>.GMP)')
    if not 0<len(data)<=1024*1024: raise ValueError('file must be 1 byte to 1 MiB')
    print(handshake(port))
    port.send(f'PUTF {name} {len(data)} {zlib.crc32(data):08x} {hashlib.sha256(data).hexdigest()}\n'.encode())
    expect(port,'READY 1024',5)
    for offset in range(0,len(data),1024):
        block=data[offset:offset+1024];port.send(block)
        expect(port,'VERIFY' if offset+len(block)==len(data) else f'ACK {offset+len(block)}',10)
    shown=f'/{name}' if name.startswith(('GAMES/','POCKET/')) else f'/OS/{name}'
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

# Serial console (docs/serial.md): typed keys go to the OS as synthetic input and as /dev/serial0 text,
# OS output ("OS <text>", "OS~ <text>" when the line continues) is shown live, kernel LOG lines dimmed.
ESCAPES={b'\x1b[A':103,b'\x1b[B':108,b'\x1b[C':106,b'\x1b[D':105}
CHAR_KEYS={b'\r':28,b'\n':28,b' ':57,b'q':16}
def unescape_os(line):
    """The bytes of one wire line "OS <text>" or "OS~ <text>", and whether the OS ended the line there."""
    whole=line.startswith('OS ');body=line[3:] if whole else line[4:];out=bytearray();i=0
    while i<len(body):
        if body[i]=='\\':
            if body[i+1:i+2]=='\\':out.append(92);i+=2
            else:out.append(int(body[i+2:i+4],16));i+=4
        else:out.append(ord(body[i]));i+=1
    return bytes(out),whole
def console_commands(data):
    """Wire commands (bytes) for typed input: arrows, Enter, Space, Q and Esc press the matching key; every
    printable key, Enter and Space also go to the OS as TX text. A lone Esc is KEY esc."""
    out=bytearray();text=bytearray();i=0
    def key(code):out.extend(f'KEY {code} down\nKEY {code} up\n'.encode())
    while i<len(data):
        seq=data[i:i+3]
        if seq in ESCAPES:key(ESCAPES[seq]);i+=3;continue
        if data[i:i+1]==b'\x1b':key(1);i+=1;continue
        ch=data[i:i+1];i+=1
        if ch in CHAR_KEYS:key(CHAR_KEYS[ch])
        if ch==b'\r':ch=b'\n'
        if ch==b'\n' or 32<=ch[0]<127:text+=ch
    for k in range(0,len(text),64):
        chunk=bytes(text[k:k+64]);out.extend(f'TX {len(chunk)}\n'.encode()+chunk)
    return bytes(out)
def console(port):
    """Two-way session: Ctrl-] ends it."""
    print(handshake(port),flush=True);print('console: arrows, Enter, Space, Q, Esc are keys; typing is text to the OS; Ctrl-] quits',file=sys.stderr)
    old=termios.tcgetattr(sys.stdin.fileno());tty.setcbreak(sys.stdin.fileno());open_line=False
    try:
        while True:
            ready=select.select([port.fd,sys.stdin.fileno()],[],[],1)[0]
            if sys.stdin.fileno() in ready:
                data=os.read(sys.stdin.fileno(),256)
                if not data or b'\x1d' in data:return 0
                port.send(console_commands(data))
            if port.fd in ready:
                try:data=os.read(port.fd,4096)
                except BlockingIOError:continue
                if not data:raise OSError('CDC disconnected')
                port.buf.extend(data)
                while b'\n' in port.buf:
                    raw,_,tail=port.buf.partition(b'\n');port.buf=bytearray(tail);line=raw.rstrip(b'\r').decode('ascii',errors='replace')
                    if line.startswith(('OS ','OS~ ')):
                        text,whole=unescape_os(line);sys.stdout.write(text.decode('utf-8',errors='replace')+('\n' if whole else ''));sys.stdout.flush();open_line=not whole
                    elif line.startswith(('OK key','OK tx')):pass
                    else:
                        if open_line:sys.stdout.write('\n');open_line=False
                        print(f'\x1b[2m{line}\x1b[0m' if sys.stdout.isatty() else '# '+line,flush=True)
    finally:termios.tcsetattr(sys.stdin.fileno(),termios.TCSADRAIN,old)

def send_command(port,command):
    """One line command (AXIS <code> <value>, INPUT add|remove) and its reply."""
    print(handshake(port));port.send((command+'\n').encode());line=port.reply(5)
    while line.startswith('INFO 2 '): line=port.reply(5)
    print(line);return 0 if line.startswith('OK') else 1

def usblog_text(parts):
    """USBLOG replies carry the file in numbered pieces with '|' for each newline; join them back."""
    return ''.join(parts).replace('|','\n')

def read_usblog(port):
    """USBLOG: the host kernel's USB.TXT, fetched piece by piece (a reply is short)."""
    print(handshake(port));parts=[];i=0;total=1
    while i<total:
        for attempt in range(4):   # the card can be busy for a moment (saving, or the OS reading): ask again
            port.send(f'USBLOG {i}\n'.encode());line=port.reply(5)
            while line.startswith(('INFO 2 ','LOG ')): line=port.reply(5)
            if line!='ERR usblog-unreadable': break
            time.sleep(0.5)
        if line.startswith('ERR '): print(line);return 1
        m=re.fullmatch(r'USBLOG (\d+)/(\d+) ?(.*)',line)
        if not m: print(line);return 1
        total=int(m.group(2))
        if total==0: print('no USB.TXT yet (boot once in keyboard mode with a device attached)');return 1
        parts.append(m.group(3));i+=1
    print(usblog_text(parts),end='');return 0

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

def stop_program(port):
    """STOP acknowledges only after kernel-boundary cleanup, not just receipt."""
    print(handshake(port));port.send(b'STOP\n');line=port.reply(10)
    while line.startswith('INFO 2 '): line=port.reply(10)
    print(line,flush=True)
    return 0 if line in ('OK stop','OK stop idle') else 1

def cec_log(port,seconds):
    """CECLOG: the newest CEC session lines (the kernel replays 12 to a new reader about a second after
    it connects, then streams new ones). The whole bounded log is /OS/SAVES/KERNEL/CEC.TXT on the card."""
    print(handshake(port),flush=True)
    end=time.monotonic()+seconds;seen=0
    while time.monotonic()<end:
        try: line=port.line(max(0.1,end-time.monotonic()))
        except TimeoutError: break
        m=re.match(r'(?:LOG|BOOT) CEC (.*)$',line)
        if m: print(m.group(1),flush=True);seen+=1
    if not seen: print('no CEC lines (is this the CEC build, and has HDMI CEC started?)',file=sys.stderr)
    return 0 if seen else 1

SHOT_MAX=4*1024*1024

def shot_header(line,kind,offset,previous=None):
    m=re.fullmatch(r'SHOT (EINK|FB)' + r' ([0-9]{1,10})'*10,line)
    if not m or m[1]!=kind: raise ValueError(f'capture reply: {line!r}')
    ident,w,h,stride,depth,order,total,got,n,crc=map(int,m.groups()[1:])
    if any(v>0xffffffff for v in (ident,w,h,stride,depth,order,total,got,n,crc)): raise ValueError('capture integer overflow')
    if not ident or order not in (0,1) or not 0<w<=4096 or not 0<h<=4096: raise ValueError('capture geometry')
    if kind=='EINK':
        if (w,h,stride,depth,total)!=(122,250,16,1,4000): raise ValueError('e-ink format')
    elif depth not in (8,16,32) or stride!=w*(depth//8) or total!=stride*h+(1024 if depth==8 else 0): raise ValueError('framebuffer format')
    if not 0<total<=SHOT_MAX or got!=offset or offset%256 or offset>=total or n!=min(256,total-offset): raise ValueError('capture bounds')
    meta=(ident,w,h,stride,depth,order,total)
    if previous is not None and meta!=previous: raise ValueError('screen mode/snapshot changed; retry capture')
    return meta,n,crc

def shot_rgb(meta,data):
    _,w,h,stride,depth,order,total=meta
    if len(data)!=total: raise ValueError('capture size')
    if depth==1:
        rgb=bytearray(250*122*3)
        for y in range(122):
            for x in range(250):
                nx,ny=(121-y,x) if order else (y,249-x)
                v=255 if data[ny*stride+nx//8]&(128>>(nx%8)) else 0
                k=(y*250+x)*3;rgb[k:k+3]=bytes((v,v,v))
        return 250,122,bytes(rgb)
    pixels=data[1024:] if depth==8 else data;rgb=bytearray(w*h*3)
    for i in range(w*h):
        if depth==8:
            c=struct.unpack_from('<I',data,pixels[i]*4)[0];r,g,b=(c>>16)&255,(c>>8)&255,c&255
        elif depth==16:
            c=struct.unpack_from('<H',pixels,i*2)[0];r,g,b=(c>>11)&31,(c>>5)&63,c&31
            r,g,b=(r*255//31,g*255//63,b*255//31)
            if order:r,b=b,r
        else:
            b,g,r=pixels[i*4:i*4+3]
            if order:r,b=b,r
        rgb[i*3:i*3+3]=bytes((r,g,b))
    return w,h,bytes(rgb)

def rgb_png(w,h,rgb):
    """Minimal RGB8 PNG: filter 0, zlib IDAT, CRC on every PNG chunk."""
    if not 0<w<=4096 or not 0<h<=4096 or len(rgb)!=w*h*3 or len(rgb)>3*SHOT_MAX: raise ValueError('PNG bounds')
    def chunk(tag,body):
        return struct.pack('>I',len(body))+tag+body+struct.pack('>I',zlib.crc32(tag+body))
    rows=b''.join(b'\0'+rgb[y*w*3:(y+1)*w*3] for y in range(h))
    return b'\x89PNG\r\n\x1a\n'+chunk(b'IHDR',struct.pack('>2I5B',w,h,8,2,0,0,0))+chunk(b'IDAT',zlib.compress(rows))+chunk(b'IEND',b'')

def screen_capture(port,path,eink=False,scale=1):
    if not 1<=scale<=16: raise ValueError('scale must be 1..16')
    print(handshake(port));kind='EINK' if eink else 'FB';meta=None;data=bytearray();offset=0
    deadline=time.monotonic()+180
    while meta is None or offset<meta[-1]:
        if time.monotonic()>deadline: raise TimeoutError('capture transfer')
        ident=meta[0] if meta else 0
        command=f'EINKSHOT {ident} {offset}' if eink else f'FBSHOT {scale} {ident} {offset}'
        port.send((command+'\n').encode())
        end=time.monotonic()+5
        while True:
            line=port.line(max(0,end-time.monotonic()))
            if line.startswith(('LOG ','OS ','OS~ ','INFO 2 ')): continue
            break
        if line=='ERR shot busy': time.sleep(0.002);continue
        meta,n,crc=shot_header(line,kind,offset,meta)
        block=port.exact(n)
        if zlib.crc32(block)!=crc: raise ValueError('capture CRC mismatch; reconnect and retry')
        data.extend(block);offset+=n
        time.sleep(0.002)
    w,h,rgb=shot_rgb(meta,data);path.write_bytes(rgb_png(w,h,rgb))
    print(f'Wrote {path} ({w}x{h})')
    print('E-ink: last sent plane; refresh, ghosting or panel failure may differ.' if eink else 'HDMI: live read; frames/palette may change during transfer; blanking is not shown.',file=sys.stderr)
    return 0

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('port');sub=ap.add_subparsers(dest='action',required=True)
    p=sub.add_parser('einkshot',help='save the last sent e-ink image as PNG');p.add_argument('out',type=pathlib.Path)
    p=sub.add_parser('fbshot',help='save the live HDMI framebuffer as RGB PNG');p.add_argument('out',type=pathlib.Path);p.add_argument('--scale',type=int,default=1)
    sub.add_parser('info');sub.add_parser('tail');p=sub.add_parser('send');p.add_argument('kernel',type=pathlib.Path)
    p=sub.add_parser('install-host',help='install a persistent host companion in the idle kernel slot');p.add_argument('kernel',type=pathlib.Path)
    p=sub.add_parser('put',help='install a file as /OS/<name>');p.add_argument('file',type=pathlib.Path);p.add_argument('--name')
    p=sub.add_parser('ceclog',help='show the newest HDMI-CEC session lines (claim result, frames, keys)');p.add_argument('--seconds',type=float,default=8)
    p=sub.add_parser('run',help='start /OS/<name> and show the log');p.add_argument('name');p.add_argument('--seconds',type=float,default=15)
    p=sub.add_parser('name',help='give this Pi a short name (1-8 of A-Z 0-9 -), shown on the e-ink and in INFO; stored as /OS/NAME.TXT');p.add_argument('text')
    sub.add_parser('usblog',help='print /OS/SAVES/KERNEL/USB.TXT: what the keyboard (host) kernel found on the USB bus last boot')
    sub.add_parser('lastboot',help='the previous boot\'s decision and keyboards found, and this boot\'s')
    sub.add_parser('stop',help='stop the OS program at a safe kernel boundary; keep recovery console')
    p=sub.add_parser('putgame',help='install a game folder as /GAMES/<DIR>/');p.add_argument('folder',type=pathlib.Path);p.add_argument('dir')
    p=sub.add_parser('putapp',help='install an app folder as /OS/APPS/<DIR>/');p.add_argument('folder',type=pathlib.Path);p.add_argument('dir')
    p=sub.add_parser('key',help='press keys on the synthetic /dev/input/event0: up down enter esc q, a code, or name:down / name:up')
    p.add_argument('keys',nargs='+');p.add_argument('--gap',type=float,default=0.3);p.add_argument('--seconds',type=float,default=2)
    p=sub.add_parser('axis',help='AXIS <code> <value> on the synthetic device');p.add_argument('code',type=int);p.add_argument('value',type=int)
    p=sub.add_parser('input',help='add or remove the synthetic device');p.add_argument('state',choices=('add','remove'))
    sub.add_parser('console',help='two-way session: typed keys and arrows go to the OS as input, OS serial output shows live, LOG lines dimmed')
    p=sub.add_parser('tone',help='TONE <hz> <ms>: quiet test tone through the mixer (HDMI audio)');p.add_argument('hz',type=int);p.add_argument('ms',type=int)
    sub.add_parser('audio',help='AUDIO: backend state, drain rate, underruns')
    a=ap.parse_args();port=Serial(a.port)
    try:
        if a.action in ('einkshot','fbshot'): return screen_capture(port,a.out,a.action=='einkshot',getattr(a,'scale',1))
        if a.action=='install-host': return send_kernel(port,a.kernel,companion=True)
        if a.action=='send': return send_kernel(port,a.kernel)
        if a.action=='put': return send_file(port,a.file,a.name or a.file.name.upper())
        if a.action=='run': return run_program(port,a.name.upper(),a.seconds)
        if a.action=='usblog': return read_usblog(port)
        if a.action=='lastboot': return send_command(port,'LASTBOOT')
        if a.action=='name':
            import tempfile
            n=a.text.upper()
            if not re.fullmatch(r'[A-Z0-9-]{1,8}',n): raise ValueError(f'{a.text!r}: a name is 1 to 8 of A-Z 0-9 -')
            with tempfile.TemporaryDirectory() as d:
                f=pathlib.Path(d)/'NAME.TXT';f.write_text(n+'\n');return send_file(port,f,'NAME.TXT')
        if a.action=='stop': return stop_program(port)
        if a.action=='ceclog': return cec_log(port,a.seconds)
        if a.action=='putgame': return send_folder(port,a.folder,a.dir.upper())
        if a.action=='putapp': return send_folder(port,a.folder,a.dir.upper(),'APPS')
        if a.action=='key': return send_keys(port,a.keys,a.gap,a.seconds)
        if a.action=='axis': return send_command(port,f'AXIS {a.code} {a.value}')
        if a.action=='console': return console(port)
        if a.action=='input': return send_command(port,f'INPUT {a.state}')
        if a.action=='tone': return send_command(port,f'TONE {a.hz} {a.ms}')
        if a.action=='audio': return send_command(port,'AUDIO')
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
