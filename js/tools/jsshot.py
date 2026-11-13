#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Turns a jsrun --shot file into a PNG (8, 16 or 32 bpp). Usage: jsshot.py <in.shot> <out.png>"""
import struct,sys,zlib
raw=open(sys.argv[1],'rb').read(); w,h,dp=struct.unpack('<4xIII',raw[:16]); px=raw[784:]
rows=bytearray()
for y in range(h):
    rows.append(0)
    for x in range(w):
        if dp==16:
            v=struct.unpack_from('<H',px,(y*w+x)*2)[0]; rows+=bytes(((v>>11)*255//31,((v>>5)&63)*255//63,(v&31)*255//31))
        elif dp==32:
            v=struct.unpack_from('<I',px,(y*w+x)*4)[0]; rows+=bytes(((v>>16)&255,(v>>8)&255,v&255))
        else: rows+=bytes((px[y*w+x],)*3)
def ch(t,d): c=struct.pack('>I',len(d))+t+d; return c+struct.pack('>I',zlib.crc32(t+d))
open(sys.argv[2],'wb').write(b'\x89PNG\r\n\x1a\n'+ch(b'IHDR',struct.pack('>IIBBBBB',w,h,8,2,0,0,0))+ch(b'IDAT',zlib.compress(bytes(rows)))+ch(b'IEND',b''))
