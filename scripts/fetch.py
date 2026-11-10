#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fetch pinned upstream inputs without invoking git or executing upstream code."""
import argparse, hashlib, io, json, pathlib, tarfile, urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1]
TINYUSB_SHA256='b7cdf35c5ccefb0f61640aff94a732ab4ebcb21498201e4a161545891b23ba01'
FIRMWARE_TAG='1.20240902'
FIRMWARE_COMMIT='b5eb52b343e9bc1391a8059a38e12e470c109f5c'
WAVESHARE_COMMIT='d28b3c4cfc78eef1c6cd6e6e3013654e449d2864'
def download(url):
    with urllib.request.urlopen(url,timeout=60) as r: return r.read()
def digest(b): return hashlib.sha256(b).hexdigest()
def manifest(dest,pin):
    records={str(p.relative_to(dest)):digest(p.read_bytes()) for p in sorted(dest.rglob('*')) if p.is_file() and p.name not in ('SHA256.json','.fetched')}
    (dest/'SHA256.json').write_text(json.dumps({'pin':pin,'files':records},indent=2)+'\n')
    (dest/'.fetched').write_text(pin+'\n')
def tinyusb():
    blob=download('https://codeload.github.com/hathach/tinyusb/tar.gz/refs/tags/0.21.0')
    if digest(blob)!=TINYUSB_SHA256: raise ValueError('TinyUSB archive hash mismatch')
    dest=ROOT/'third_party/tinyusb';dest.mkdir(parents=True,exist_ok=True)
    with tarfile.open(fileobj=io.BytesIO(blob),mode='r:gz') as tar:
        for entry in tar.getmembers():
            name=entry.name.partition('/')[2]
            allowed=name=='LICENSE' or name in ('src/tusb.c','src/tusb.h','src/tusb_option.h','src/osal/osal.h','src/osal/osal_none.h') or name.startswith(('src/common/','src/device/','src/class/cdc/','src/class/hid/','src/host/','src/portable/synopsys/dwc2/'))
            if allowed and entry.isfile() and '..' not in pathlib.PurePosixPath(name).parts:
                path=dest/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(tar.extractfile(entry).read())
    manifest(dest,'TinyUSB 0.21.0 archive SHA256 '+TINYUSB_SHA256)
def firmware():
    dest=ROOT/'third_party/firmware';dest.mkdir(parents=True,exist_ok=True)
    lock=json.loads((ROOT/'scripts/firmware-lock.json').read_text())
    for name,expected in lock.items():
        path=dest/name
        blob=path.read_bytes() if path.exists() else download(f'https://raw.githubusercontent.com/raspberrypi/firmware/{FIRMWARE_COMMIT}/boot/{name}')
        if digest(blob)!=expected: raise ValueError('Firmware hash mismatch: '+name)
        path.write_bytes(blob)
    manifest(dest,FIRMWARE_TAG+' '+FIRMWARE_COMMIT)
if __name__=='__main__':
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('dependency',choices=['tinyusb','firmware']);a=ap.parse_args();globals()[a.dependency]()
