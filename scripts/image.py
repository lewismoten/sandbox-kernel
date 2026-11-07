#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""M0 manifest and deterministic MBR/FAT32 image. All integers little endian."""
import argparse, hashlib, json, pathlib, struct, zlib
MAX_IMAGE=2*1024*1024
IMAGE_MAGIC=0x31494b53
CONTROL_MAGIC=0x31434b53
NO_SLOT=0xffffffff

def pack(payload,build_id):
    build=build_id.encode('ascii')
    if not 4<=len(payload)<=MAX_IMAGE-256 or len(build)>31 or any(c<32 or c>126 for c in build): raise ValueError('payload/build ID')
    header=struct.pack('<8I',IMAGE_MAGIC,1,1,0x10000,len(payload),0x100000,0x100000,0)
    return header+hashlib.sha256(payload).digest()+build.ljust(32,b'\0')+bytes(160)+payload

def validate(image):
    if not 260<=len(image)<=MAX_IMAGE: raise ValueError('size')
    if struct.unpack_from('<8I',image)!=(IMAGE_MAGIC,1,1,0x10000,len(image)-256,0x100000,0x100000,0) or any(image[96:256]) or hashlib.sha256(image[256:]).digest()!=image[32:64]: raise ValueError('manifest/hash')
    build=image[64:96]
    if b'\0' not in build or any(build[build.index(0):]) or any(c<32 or c>126 for c in build[:build.index(0)]): raise ValueError('build ID')
    return image

def control_record(image,sequence=1):
    validate(image)
    buf=bytearray(struct.pack('<128I',*([CONTROL_MAGIC,1,sequence,0,NO_SLOT,0,1,0,NO_SLOT]+[0]*119)))
    buf[64:96]=hashlib.sha256(image).digest();struct.pack_into('<I',buf,508,zlib.crc32(buf[:508]));return bytes(buf)

def short_name(name):
    parts=name.upper().split('.')
    if len(parts)>2 or len(parts[0])>8 or (len(parts)==2 and len(parts[1])>3): raise ValueError('not 8.3: '+name)
    return (parts[0].ljust(8)+(parts[1] if len(parts)==2 else '').ljust(3)).encode('ascii')

def fat_image(folder,output):
    start,total,reserved,fat_sectors=2048,262144,32,2048
    data=start+reserved+2*fat_sectors;clusters=start+total-data;fat=bytearray(fat_sectors*512)
    def entry(c,v): struct.pack_into('<I',fat,c*4,v)
    entry(0,0x0ffffff8);entry(1,0xffffffff)
    files={p.name:p.read_bytes() for p in folder.iterdir() if p.is_file() and p.suffix.lower() not in ('.json','.md')}
    order=['BOOTCT0.BIN','KERNA.BIN','BOOTCT1.BIN','KERNB.BIN']+sorted(set(files)-{'BOOTCT0.BIN','KERNA.BIN','BOOTCT1.BIN','KERNB.BIN'})
    if len(order)>24: raise ValueError('root directory full')
    root_index=0
    root=bytearray(1024);entry(2,3);entry(3,0x0fffffff);next_cluster=4;layout={}
    with output.open('wb') as out:
        out.truncate((start+total)*512)
        def sector(lba,b): out.seek(lba*512);out.write(b)
        mbr=bytearray(512);mbr[450]=0x0c;struct.pack_into('<II',mbr,454,start,total);mbr[510:]=b'\x55\xaa';sector(0,mbr)
        boot=bytearray(512);boot[:11]=b'\xeb\x58\x90SANDBOX '
        struct.pack_into('<HBHBHHBHHHII',boot,11,512,1,reserved,2,0,0,0xf8,0,63,255,start,total)
        struct.pack_into('<IHHIHH',boot,36,fat_sectors,0,0,2,1,6)
        boot[64]=0x80;boot[66]=0x29;struct.pack_into('<I',boot,67,0x53424b30);boot[71:82]=b'SANDBOX M0 ';boot[82:90]=b'FAT32   ';boot[510:]=b'\x55\xaa';sector(start,boot);sector(start+6,boot)
        fsinfo=bytearray(512);struct.pack_into('<I',fsinfo,0,0x41615252);struct.pack_into('<III',fsinfo,484,0x61417272,NO_SLOT,NO_SLOT);struct.pack_into('<I',fsinfo,508,0xaa550000);sector(start+1,fsinfo);sector(start+7,fsinfo)
        for i,name in enumerate(order):
            content=files[name];count=(len(content)+511)//512
            if not count: continue
            if next_cluster+count>clusters+2: raise ValueError('image full')
            c=next_cluster;next_cluster+=count
            for j in range(count): entry(c+j,c+j+1 if j+1<count else 0x0fffffff)
            try: alias=short_name(name)
            except ValueError:
                alias=short_name('LICENC~1.BRO')
                checksum=0
                for b in alias: checksum=(((checksum&1)<<7)+(checksum>>1)+b)&255
                chars=list(name.encode('utf-16le'))+[0,0]
                while len(chars)%26: chars += [255,255]
                chunks=[bytes(chars[j:j+26]) for j in range(0,len(chars),26)]
                for j in reversed(range(len(chunks))):
                    e=bytearray(32);e[0]=(j+1)|(0x40 if j==len(chunks)-1 else 0);e[11]=15;e[13]=checksum
                    e[1:11]=chunks[j][:10];e[14:26]=chunks[j][10:22];e[28:32]=chunks[j][22:26]
                    root[root_index*32:(root_index+1)*32]=e;root_index+=1
            if root_index>=31: raise ValueError('root entries full')
            ent=memoryview(root)[root_index*32:(root_index+1)*32];root_index+=1
            ent[:11]=alias;ent[11]=0x20;struct.pack_into('<H',ent,20,c>>16);struct.pack_into('<HI',ent,26,c&0xffff,len(content))
            sector(data+c-2,content.ljust(count*512,b'\0'))
            layout[name]={'first_lba':data+c-2,'sectors':count,'bytes':len(content),'sha256':hashlib.sha256(content).hexdigest()}
        sector(data,root);sector(start+reserved,fat);sector(start+reserved+fat_sectors,fat)
        struct.pack_into('<II',fsinfo,488,clusters-(next_cluster-2),next_cluster)
        sector(start+1,fsinfo);sector(start+7,fsinfo)
    output.with_suffix('.layout.json').write_text(json.dumps({'format':1,'sector_bytes':512,'partition_start':start,'partition_sectors':total,'files':layout},indent=2)+'\n')

if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);sub=ap.add_subparsers(dest='action',required=True)
    p=sub.add_parser('pack');p.add_argument('input',type=pathlib.Path);p.add_argument('output',type=pathlib.Path);p.add_argument('--build-id',default='m0-dev')
    p=sub.add_parser('fat');p.add_argument('folder',type=pathlib.Path);p.add_argument('output',type=pathlib.Path)
    a=ap.parse_args()
    if a.action=='pack': a.output.write_bytes(pack(a.input.read_bytes(),a.build_id))
    else: fat_image(a.folder,a.output)
