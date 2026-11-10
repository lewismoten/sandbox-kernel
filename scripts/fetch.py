#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Fetch pinned upstream inputs without invoking git or executing upstream code."""
import argparse, hashlib, io, json, pathlib, shutil, tarfile, urllib.request
ROOT=pathlib.Path(__file__).resolve().parents[1]
TINYUSB_VERSION='0.21.0'   # the Makefile's TUSB_VERSION must match (tests/test_fetch.py)
TINYUSB_SHA256='b7cdf35c5ccefb0f61640aff94a732ab4ebcb21498201e4a161545891b23ba01'
FIRMWARE_TAG='1.20240902'
FIRMWARE_COMMIT='b5eb52b343e9bc1391a8059a38e12e470c109f5c'
WAVESHARE_COMMIT='d28b3c4cfc78eef1c6cd6e6e3013654e449d2864'
def download(url):
    with urllib.request.urlopen(url,timeout=60) as r: return r.read()
def digest(b): return hashlib.sha256(b).hexdigest()
def manifest(dest,pin,stamps=('.fetched',)):
    records={str(p.relative_to(dest)):digest(p.read_bytes()) for p in sorted(dest.rglob('*')) if p.is_file() and not p.name.startswith('.fetched') and p.name!='SHA256.json'}
    (dest/'SHA256.json').write_text(json.dumps({'pin':pin,'files':records},indent=2)+'\n')
    for stamp in stamps: (dest/stamp).write_text(pin+'\n')
def install_tinyusb(blob,dest,version,pin):
    """Extract the selected closure into a fresh sibling directory, then swap it in, so nothing
    from an older version survives and the manifest describes only this archive."""
    dest=pathlib.Path(dest);stage=dest.with_name(dest.name+'.new');shutil.rmtree(stage,ignore_errors=True);stage.mkdir(parents=True)
    with tarfile.open(fileobj=io.BytesIO(blob),mode='r:gz') as tar:
        for entry in tar.getmembers():
            name=entry.name.partition('/')[2]
            allowed=name=='LICENSE' or name in ('src/tusb.c','src/tusb.h','src/tusb_option.h','src/osal/osal.h','src/osal/osal_none.h') or name.startswith(('src/common/','src/device/','src/class/cdc/','src/class/hid/','src/host/','src/portable/synopsys/dwc2/'))
            if allowed and entry.isfile() and '..' not in pathlib.PurePosixPath(name).parts:
                path=stage/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_bytes(tar.extractfile(entry).read())
    manifest(stage,pin,('.fetched','.fetched-'+version))
    shutil.rmtree(dest,ignore_errors=True);stage.rename(dest)
def tinyusb():
    blob=download('https://codeload.github.com/hathach/tinyusb/tar.gz/refs/tags/'+TINYUSB_VERSION)
    if digest(blob)!=TINYUSB_SHA256: raise ValueError('TinyUSB archive hash mismatch')
    install_tinyusb(blob,ROOT/'third_party/tinyusb',TINYUSB_VERSION,'TinyUSB '+TINYUSB_VERSION+' archive SHA256 '+TINYUSB_SHA256)
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
