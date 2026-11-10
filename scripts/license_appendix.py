#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Regenerate the per-file notice appendix of licenses/TinyUSB.txt from the fetched tree.
Every retained file's leading comment is copied verbatim; files with identical notices share one block."""
import pathlib, re
ROOT=pathlib.Path(__file__).resolve().parents[1]
SRC=ROOT/'third_party/tinyusb'
OUT=ROOT/'licenses/TinyUSB.txt'
MARK='Per-file upstream notices retained below.'
def header(text):
    m=re.match(r'\s*(/\*.*?\*/)',text,re.S)
    return m.group(1) if m else ''
def second(text):
    """A second notice right after the first (dwc2_type.h carries an STMicroelectronics one)."""
    m=re.match(r'\s*/\*.*?\*/\s*(/\*\*?.*?\*/)',text,re.S)
    return m.group(1) if m and re.search(r'copyright|licen[sc]e',m.group(1),re.I) else ''
def main():
    groups={}
    for p in sorted((SRC/'src').rglob('*')):
        if p.suffix in ('.c','.h'):
            groups.setdefault(header(p.read_text(errors='replace')),[]).append(str(p.relative_to(SRC)))
    old=OUT.read_text();head=old[:old.index(MARK)]
    body=MARK+' Only the selected source closure is linked; the rest is fetched, not built.\n'
    extra={}
    for p in sorted((SRC/'src').rglob('*')):
        if p.suffix in ('.c','.h'):
            n=second(p.read_text(errors='replace'))
            if n: extra.setdefault(n,[]).append(str(p.relative_to(SRC)))
    for h,files in sorted(groups.items(),key=lambda kv:kv[1][0]):
        body+='\nFiles: '+', '.join(files)+'\n'+(h or '(no leading notice in these files)')+'\n'
    for n,files in sorted(extra.items(),key=lambda kv:kv[1][0]):
        body+='\nSecond notice, also carried by '+', '.join(files)+' (third party; BSD 3-Clause as stated in the notice):\n'+n+'\n'
    OUT.write_text(head+body)
if __name__=='__main__': main()
