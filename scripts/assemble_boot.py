#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Assemble boot folder and a reproducible sparse card image; never flash media."""
import hashlib, json, pathlib, shutil
from fetch import ROOT, firmware, FIRMWARE_TAG, FIRMWARE_COMMIT
from image import MAX_IMAGE, validate, control_record, fat_image

def refuse_host_role(stamp=None):
    """R1: the factory image is confirmed slot A, and a host-role image never confirms; refuse it here."""
    stamp=stamp or ROOT/'build/usb_role.stamp'
    role=stamp.read_text().strip() if stamp.exists() else 'device'
    if role!='device': raise SystemExit('refusing to assemble a card image from a USB_ROLE=%s runtime: install a host companion from a device-role kernel instead'%role)

def main():
    refuse_host_role()
    firmware()
    build=ROOT/'build';dest=build/'boot';dest.mkdir(parents=True,exist_ok=True)
    # All output names fit FAT 8.3. No DTBs or GPL firmware-side kernel payloads.
    for name in ('bootcode.bin','start.elf','fixup.dat','LICENCE.broadcom'):
        shutil.copyfile(ROOT/'third_party/firmware'/name,dest/name)
    shutil.copyfile(build/'kernel.img',dest/'kernel.img')
    image=validate((build/'runtime.ski').read_bytes())
    (dest/'KERNA.BIN').write_bytes(image.ljust(MAX_IMAGE,b'\0'))
    (dest/'KERNB.BIN').write_bytes(bytes(MAX_IMAGE))
    record=control_record(image)
    (dest/'BOOTCT0.BIN').write_bytes(record+bytes(4096-512))
    (dest/'BOOTCT1.BIN').write_bytes(record+bytes(4096-512))
    (dest/'config.txt').write_text('kernel=kernel.img\nkernel_address=0x8000\narm_64bit=0\ndevice_tree=\ngpu_mem=32\ncore_freq=250\ncore_freq_min=250\nforce_turbo=0\nenable_uart=1\ninit_uart_clock=3000000\nhdmi_force_hotplug=1\nhdmi_group=1\nhdmi_mode=16\nhdmi_drive=2\nhdmi_ignore_cec=1\nhdmi_ignore_cec_init=1\ndisable_overscan=1\nscaling_kernel=8\n')  # hdmi_ignore_cec=1: the VideoCore firmware's own CEC task stays off the bus, so the kernel's driver (drivers/cec_hw.c) is the only device answering as a playback device (the TV showed the firmware's name, "raspberry"); hdmi_ignore_cec_init=1 also drops the firmware's start-up Active Source, which would otherwise race the kernel's own announcement (docs/cec.md); hdmi_drive=2: HDMI (not DVI) signaling, which carries the audio packets the kernel's HDMI audio backend sends (docs/audio-device.md); hdmi_*: force HDMI on at 1920x1080 60 Hz (CEA mode 16) even when no display is detected at power-on; the GPU scales each mode to it with nearest-neighbor (scaling_kernel=8), so 960x540 is exactly 2x
    # Full notices at the root; .txt names fit 8.3.
    shutil.copyfile(ROOT/'LICENSE',dest/'MIT.TXT')
    shutil.copyfile(ROOT/'licenses/TinyUSB.txt',dest/'TINYUSB.TXT')
    shutil.copyfile(ROOT/'licenses/Waveshare.txt',dest/'WAVE.TXT')
    shutil.copyfile(ROOT/'licenses/Unicode.txt',dest/'UNICODE.TXT')
    inventory={'firmware_tag':FIRMWARE_TAG,'firmware_commit':FIRMWARE_COMMIT,'files':{p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(dest.iterdir()) if p.is_file() and p.suffix!='.json'}}
    (dest/'SHA256.json').write_text(json.dumps(inventory,indent=2)+'\n')
    fat_image(dest,build/'sandbox-m0.img')
    print('Assembled build/boot and build/sandbox-m0.img (no device writes)')
if __name__=='__main__': main()
