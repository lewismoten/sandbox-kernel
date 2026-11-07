# SPDX-License-Identifier: MIT
"""Sparse FAT32 test images with subdirectories, for the asset reader tests.

build(path, tree, spc=1, fragment=False) writes a 2048+262144-sector card
(the harness reports 264192 sectors) and returns a Volume describing where
everything landed, so a test can corrupt exact FAT entries or directory bytes.
tree maps 8.3 names to bytes (files) or nested dicts (directories)."""
import struct
START,TOTAL,RESERVED,FAT_SECTORS=2048,262144,32,2048
EOC=0x0fffffff

def short(name):
    base,_,ext=name.partition('.')
    return (base.ljust(8)+ext.ljust(3)).encode('ascii')

class Volume:
    def __init__(self,spc,fragment):
        self.spc=spc;self.fragment=fragment
        self.fat_lba=START+RESERVED;self.data_lba=self.fat_lba+2*FAT_SECTORS
        self.clusters=(START+TOTAL-self.data_lba)//spc
        self.fat={0:0x0ffffff8,1:0xffffffff};self.sectors={};self.next=2
        self.chains={};self.entries={}
    def alloc(self,count):
        out=[]
        for _ in range(count):
            out.append(self.next);self.next+=2 if self.fragment else 1
        for a,b in zip(out,out[1:]+[None]): self.fat[a]=b if b else EOC
        return out
    def lba(self,cluster): return self.data_lba+(cluster-2)*self.spc
    def put(self,chain,data):
        cb=self.spc*512;data=data.ljust(len(chain)*cb,b'\0')
        for i,c in enumerate(chain):
            for j in range(self.spc): self.sectors[self.lba(c)+j]=data[i*cb+j*512:i*cb+j*512+512]
    def entry(self,name,attr,cluster,size):
        e=bytearray(32);e[:11]=short(name) if name not in ('.','..') else name.encode().ljust(11);e[11]=attr
        struct.pack_into('<H',e,20,cluster>>16);struct.pack_into('<HI',e,26,cluster&0xffff,size);return bytes(e)
    def directory(self,path,tree,parent,root_extras=()):
        cb=self.spc*512
        count=max(1,-(-(32*(len(tree)+2+len(root_extras)+1))//cb))
        chain=self.alloc(count);self.chains[path or '/']=chain
        raw=list(root_extras)
        if path: raw+= [self.entry('.',0x10,chain[0],0),self.entry('..',0x10,0 if parent==2 else parent,0)]
        for name,item in tree.items():
            full=f'{path}/{name}'
            if isinstance(item,dict):
                sub=self.directory(full,item,chain[0]);raw.append(self.entry(name,0x10,sub,0))
            else:
                n=-(-len(item)//cb);c=self.alloc(n) if n else []
                self.chains[full]=c
                if c: self.put(c,item)
                raw.append(self.entry(name,0x20,c[0] if c else 0,len(item)))
            self.entries[full]=(chain,len(raw)-1)
        self.put(chain,b''.join(raw))
        return chain[0]
    def entry_offset(self,full):
        """Card byte offset of a directory entry."""
        chain,index=self.entries[full];cb=self.spc*512
        return self.lba(chain[index*32//cb])*512+index*32%cb
    def fat_offset(self,cluster,copy=0): return (self.fat_lba+copy*FAT_SECTORS)*512+cluster*4

def build(path,tree,spc=1,fragment=False,extras=True):
    v=Volume(spc,fragment)
    label=v.entry('TESTVOL',0x08,0,0)
    deleted=bytearray(v.entry('GONE.TXT',0x20,0,0));deleted[0]=0xe5
    lfn=bytearray(32);lfn[0]=0x41;lfn[11]=0x0f;lfn[1:11]=b'l\0o\0n\0g\0\0\0'
    v.directory('',tree,0,(label,bytes(deleted),bytes(lfn)) if extras else ())
    fat=bytearray(FAT_SECTORS*512)
    for c,val in v.fat.items(): struct.pack_into('<I',fat,c*4,val)
    with open(path,'wb') as f:
        f.truncate((START+TOTAL)*512)
        mbr=bytearray(512);mbr[450]=0x0c;struct.pack_into('<II',mbr,454,START,TOTAL);mbr[510:]=b'\x55\xaa'
        f.seek(0);f.write(mbr)
        boot=bytearray(512);boot[:11]=b'\xeb\x58\x90SANDBOX '
        struct.pack_into('<HBHBHHBHHHII',boot,11,512,spc,RESERVED,2,0,0,0xf8,0,63,255,START,TOTAL)
        struct.pack_into('<IHHIHH',boot,36,FAT_SECTORS,0,0,2,1,6);boot[510:]=b'\x55\xaa'
        f.seek(START*512);f.write(boot)
        for copy in (0,1): f.seek((v.fat_lba+copy*FAT_SECTORS)*512);f.write(fat)
        for lba,data in v.sectors.items(): f.seek(lba*512);f.write(data)
    return v

def patch(path,offset,data):
    with open(path,'r+b') as f: f.seek(offset);f.write(data)

def set_fat(path,v,cluster,value):
    for copy in (0,1): patch(path,v.fat_offset(cluster,copy),struct.pack('<I',value))
