# USB keyboard mode with an installed host companion

The normal kernel provides the Mac USB link. A separate `USB_ROLE=host` kernel
can stay installed in the idle A/B slot as a **host companion**. On each power-on,
the normal kernel samples the OTG adapter's ID pin. Grounded ID and no enumerated
USB device link select the companion for that boot. The debounce is 300 ms and the boot decision
window is 1.5 s (`USBROLE_HOST_STABLE_US`, `USBROLE_DECISION_WINDOW_US`; the window must clearly outlast the
debounce, which the first hardware test on 2026-11-10 showed). No adapter or a Mac link is decided at once. No confirmed slot,
trial attempt or boot-control record is changed by this handoff. Without the
adapter, the next power-on runs the normal kernel and its Mac link again.

This automatic handoff and the host stack still need hardware qualification.
Connect the adapter **before powering on**. Plugging it into an already running
kernel does not select keyboard mode: ID is sampled only at boot. Unplugging a
keyboard releases its keys, but returning to the Mac link requires powering off,
removing the OTG adapter, and powering on with the normal Mac data cable.

## Install once from a Mac

1. Connect the Pi to the Mac with its normal USB data cable, without the OTG
   adapter. Run these commands from the repository folder. Replace
   `/dev/cu.usbmodemXXXX` with the Pi's actual port (`ls /dev/cu.usbmodem*`).
2. Update the normal device kernel first, then let its 30-second trial confirm:

   ```sh
   make USB_ROLE=device all
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX send build/runtime.ski
   # Reconnect after the reboot; wait at least 30 seconds.
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX info
   ```

   INFO must show `pending=0`, `updates=ready`, and `host_companion=0`.
   If a companion is already installed, `host_companion` may be nonzero.
3. Build and install the keyboard kernel:

   ```sh
   make USB_ROLE=host all
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX install-host build/runtime.ski
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX info
   ```

   Wait for `OK host companion installed`. INFO shows a nonzero
   `host_companion` generation and still shows `pending=0`. Installation does
   not reboot or consume a trial. If the final reply is lost, reconnect and
   inspect INFO before deciding whether to resend.
4. Power off. Disconnect the Mac data cable, attach the OTG adapter and keyboard
   receiver, and power the Pi through its power connector. The normal kernel
   starts first, then hands off to the keyboard kernel. Look for `HOST: keyboard`
   on e-ink and `keyboard ready` on HDMI.
5. To use the Mac link again, power off, remove the adapter, reconnect the normal
   data cable, and power on. Repeat step 4 whenever keyboard mode is wanted;
   the companion remains installed across power cycles.

`USB: OTG: INSTALL KBD` means the keyboard kernel is absent or could not be
validated. Return to the Mac cable and run step 3. The status text uses all 16
available e-ink columns without truncation. UART/USB logs give the longer action.

There are only two kernel slots. A normal kernel update replaces the companion;
install it again after the new device kernel confirms. An interrupted companion
installation can remove keyboard availability, while preserving the confirmed
normal kernel. `make boot` continues to refuse host images as factory defaults.
Use `install-host`, rather than `send`, to keep a keyboard kernel installed.

## Boot and update protections

The image manifest's word 7 identifies device/legacy (0) or host (1). The installer
requires a tagged host image for `PUT2 host-companion`; arbitrary raw code is not
authenticated by this tag. Transport CRC, payload SHA, full image hash and sector
readback checks remain in force. The companion reference is committed only after
readback, separately from pending trial metadata. Before either kind of kernel
installation overwrites the idle slot, an old companion reference is revoked by a
verified metadata commit.

The selector still starts the confirmed device kernel on an ordinary boot. That
kernel verifies the confirmed fallback and companion, rechecks the live USB-link
decision after card reads, deinitializes device USB, and uses the existing
selector handoff record at `0x7000` with boot kind 2. A 72-byte relocated ARM
trampoline at `0x7800` masks interrupts, cleans caches, disables MMU/caches, copies
the verified payload to `0x100000`, and enters the normal runtime startup. The SD
handoff block is refreshed too. This requires no selector replacement on an
existing card and leaves no persistent boot request to survive a power cycle.
The handoff happens before audio or OS startup.

Host confirmation code is excluded at compile time. A host kernel refuses a
confirmed/recovery entry (boot kind 0); it accepts consumed trials (1) or a
matching installed-companion handoff (2). The latter rechecks its slot/generation
against metadata before starting host USB and is never treated as a confirmable
trial. Installed companions remain idle when the adapter is absent. Legacy
one-shot host trials remain a separate mechanism; tagged trials require a
selector that understands the role word, whereas companion handoff does not.
If both control records are lost or the confirmed device image is corrupt,
normal card-reader rescue rules still apply; the companion is not a fallback
for the Mac recovery link. See [M0 A/B update format](m0.md).

## Boot-time ID detection

## Register facts
- GOTGCTL at 0x20980000 (base 0x20980000, GUSBCFG at +0x0C as used in drivers/usb.c).
- GOTGCTL.ConIDSts = bit 16, 1 = B-device, 0 = A-device: Linux `drivers/usb/dwc2/hw.h`,
  `GOTGCTL_CONID_B BIT(16)`. Checked 2026-11-09.
- GUSBCFG.ForceHstMode = bit 29, ForceDevMode = bit 30: same file.
- Forced mode hides the real pin: Linux `dwc2_clear_force_mode()` comment says the
  connector ID status "is affected by the force mode" and allows up to 100 ms of IDDIG
  filter delay after clearing the bits (Linux is a source of facts only; no code used).
- VERIFY: whether the pin reads right before TinyUSB's core reset and PHY setup; the
  Zero W board routes the OTG connector ID pin to the core (believed, not checked).

## The ID sampling window (review R1) and what is guaranteed
Options considered for reading ID without leaving device mode:
- (a) Read ConIDSts, GINTSTS.CurMod or a PHY/GPIO path while ForceDevMode is set. The
  Linux comment above says forcing changes ConIDSts, so ConIDSts is not expected to show the
  pin; CurMod reports the forced mode, not the pin. No GPIO path to the ID pin is known
  (the BCM2835 ID pin is not a GPIO; not checked in the schematic: VERIFY). Rejected as
  unproven.
- (b) Keep ForceDevMode set and clear only ForceHstMode: ForceDevMode itself is what
  changes ConIDSts, so this is not expected to help. Rejected as unproven.
- (c) Chosen: keep the window, make it narrow and checked.

The window (`usb_init`) clears both force bits for 100 ms, once, at boot, then forces
device. During it the code:
- opens the window only if HPRT0.PrtPwr (bit 12) is clear, else it skips it and stays
  device;
- reads HPRT0 (once, to decide) and GOTGCTL (once, after the 100 ms); it writes only GUSBCFG
  (clears both force bits, then forces device). It does not write HPRT0, GHWCFG or any
  host register, never sets ForceHstMode, never starts host init or a soft reset, and the core interrupt handler is not enabled yet
  (`usb_irq` returns until `initialized`);
- forces device straight after (`usbrole_device_gusbcfg`; tested in tests/usbrole.c).

Guarantee for the device-role image and its sampling window, precisely: that code never sets ForceHstMode and never writes HPRT0 (so it never
sets the port power bit or starts host init). The role is not "device for every
microsecond": for about
100 ms at boot the core is in OTG mode, not forced device, so a grounded ID could make the
core report A-device on its own. The kernel does nothing in that state that would drive VBUS:
the port power bit is the software control and it is never set. Whether the core itself
could assert a VBUS-drive pin in that state is VERIFY on hardware. Whether the Pi Zero W's
OTG VBUS is switched by the core or hard-wired to 5 V is also VERIFY: no schematic was
found in this repository; check the Raspberry Pi Zero W schematic before relying on it.
So "device-only" means no host-mode initialization, not that the core is in device mode for
every microsecond.

## Timing (review R2)
`usbrole_step` takes the raw 32-bit `micros()` and compares unsigned microsecond deltas
(500,000 us). `micros()` wraps at 2^32 us (71.6 min); dividing it by 1000 first broke the
debounce at the wrap. Tests cover the wrap and exactly 500 ms across it. (TinyUSB's own
`tusb_time_millis_api` still does `micros()/1000`, so its millisecond counter jumps at the
wrap; not changed here.)
- Hot-plug is not implemented: the ID level is sampled once at boot and latched; only the
  "no computer enumerated" half of the decision is live (300 ms debounce, `usbrole_step`).
  Live hot-plug sampling needs a brief un-force window (or the A/B-device change
  interrupt); that disturbs a gadget link, so it is slice 2.

## Host implementation

The build uses the pinned MIT-licensed TinyUSB DWC2 host backend; no Linux or
FreeBSD USB code is copied. Hub support and live role switching remain future
work. The default device build never initializes host USB itself: only the
verified companion starts that stack.

## Slice 1: descriptor walker and boot-report parser (host-tested, no hardware)

- `core/usbdesc.c`: `usbdesc_find_boot_keyboard` walks a configuration descriptor and returns the
  HID boot keyboard interface (class 3, subclass 1, protocol 1, alternate 0) with its interrupt IN
  endpoint (max packet at least 8), wherever it sits on a composite device. A bad `bLength`, an
  overrun or a truncated buffer returns `USBDESC_E_MALFORMED`; a good descriptor without a keyboard
  returns `USBDESC_E_NOT_FOUND`.
- `core/hidboot.c`: `hidboot_diff` turns each 8-byte report into key down/up changes against the
  previous one (Linux key codes, releases first); a rollover-error report is ignored.
  `hidboot_feed` sends them to `input_frame` in frames of at most `INPUT_FRAME_MAX`.
  `hidboot_release_all` releases held keys on unplug.
- Tests: `tests/hidboot.c`, run by `tests/test_hidboot.py`. The host-role glue
  now calls the report parser. The descriptor walker remains a standalone core;
  TinyUSB performs runtime enumeration. The OS-facing ABI does not change.

## Slice 2: the host-role build (keyboard on the OTG port)

Build: `make USB_ROLE=host all`. The default `USB_ROLE=device` is the Mac link.
A role change relinks everything through `build/usb_role.stamp`. Each runtime
has one fixed USB role; the automatic boot decision selects between images.
There is no CDC link in a host image. It has one hub (`CFG_TUH_HUB 1`, up to four devices behind it) and takes up to
`USBKBD_MAX` (4) boot keyboards at once on any device; a receiver's mouse and consumer interfaces are skipped.

## Boot record, name and the e-ink

Every boot writes one line to `/OS/SAVES/KERNEL/BOOT.TXT`: `n<boot counter>,<decision>,k<keyboards found>`
(decisions: `link`, `no-adapter`, `install-kbd`, `keyboard`). The e-ink USB line says `KEYBOARD MODE` (plus the
number of keyboards once found, host role), `NO OTG ADAPTER`, or `OTG: INSTALL KBD`. The host kernel rewrites
the line once the keyboard count has held for 2 s. `INFO` adds `name= boot= lastboot=` (the previous boot's
record) and the `LASTBOOT` command prints them; `usb_kernel.py lastboot` sends it. A Pi's short name
(`usb_kernel.py name PI-2`, 1 to 8 of A-Z 0-9 -, `/OS/NAME.TXT`, ignored if invalid) is drawn in the status screen's
bottom-right corner and shown in INFO.

What is in it: TinyUSB host (`usbh.c`, `hid_host.c`, `hcd_dwc2.c`), slave (PIO) mode, one
device, HID only. `drivers/usb_host.c` replaces `drivers/usb.c`; `core/usbkbd.c` is the
host-testable glue. TinyUSB sets boot protocol on a boot-subclass HID interface before its
mount callback; the glue arms the interrupt endpoint (a refused submit is retried three times, then the keyboard is removed: keys released, status `HOST: device, no keyboard`), `hidboot_feed` turns reports into
`input_frame` changes, and an unmount calls `hidboot_release_all` then `input_remove`.
`include/tusb_config.h` carries the host settings and a one-line `dwc2_int_set` shim,
because TinyUSB 0.21's `dwc2_bcm.h` has only the device interrupt switch.

What the HDMI console shows, in order (the e-ink status line shows `HOST: no device`,
`HOST: device, no keyboard` or `HOST: keyboard`):
1. `USB host up`: the stack started (it did not hang before this).
2. `device attached VID:PID`: the port reset and enumeration reached the device descriptor.
3. `keyboard ready`: a boot keyboard interface mounted and polling started. If the device has
   no boot keyboard interface the line is `HID interface skipped (not a boot keyboard)`.
4. `key NAME` for each press (`key Up`, `key Space`, `key LShift`); releases are not shown.
5. `keyboard removed` / `device removed` when it is unplugged.
Stopping at 1 with a device plugged in means the port never saw it (power or reset); stopping at
2 means set-up or HID mounting failed.

Tests: `tests/usbkbd.c` (`make test`) drives the glue with fakes: stage lines, skipped
non-keyboard interfaces, press names, no stuck key on unplug, replug, rollover and short reports.

Unverified on hardware: whether TinyUSB's `hcd_dwc2.c` runs on the Zero's BCM2835 core at all,
the FIFO split, VBUS being powered, and the keyboard dongle being full speed.

## Composite dongles and start-up (review R2, R3)

`CFG_TUH_HID` counts interfaces, so it is 4: a mouse or consumer-control interface before or after
the keyboard no longer uses up the slot. The first boot-protocol keyboard interface is the one
polled; its (device, interface) pair is remembered, so reports from, and the unmount of, any other
interface are ignored. A keyboard that offers no boot protocol is skipped. `input_init()` now runs
before USB or the SD card can deliver a report, so a key held at mount is kept.

## Hardware acceptance for automatic keyboard mode

- Install the new device kernel and companion over the Mac cable. Record INFO's
  confirmed generation, companion generation and `pending=0`.
- Cold boot with the adapter and receiver attached: confirm handoff, `USB host up`,
  attachment VID:PID, `keyboard ready`, key presses, held-key release on unplug,
  and keyboard replug. Inspect the two startup screens and watchdog behavior.
- Repeat cold boots with the adapter several times without reinstalling anything.
  Then boot without it and verify the Mac enumerates, INFO still has the same
  confirmed/companion generations, and the host never confirmed after 30 seconds.
- Test floating ID, grounded ID with an enumerated link where electrically
  appropriate, and a receiver that is not a boot-protocol keyboard. A short link
  loss must not cause a handoff. Verify actual ID sensing, port power/VBUS, DWC2
  reset after device deinit, and SD adoption across the in-memory handoff.
- With no companion, confirm `USB: OTG: INSTALL KBD` fits; with a damaged companion,
  confirm device recovery remains available on the next boot without the adapter.
- Interrupt installation/replacement at slot write and metadata commit boundaries;
  boot without the adapter and confirm the Mac fallback is intact. Perform a
  normal device update, confirm it, then reinstall the companion.

Host tests exercise the role debounce (including timer wrap), link priority,
missing-companion action/width, trial exclusion, real installer/readback/hash
and metadata persistence/revocation. They do not execute ARM cache/MMU transitions,
USB registers or physical power cuts.

## The USB log (USB.TXT)

In the host role the kernel keeps a short text record of the bus and saves it to
`/OS/SAVES/KERNEL/USB.TXT` (replaced each boot, at most 1600 bytes plus an events line, written
once the log has been quiet for 1.5 s, at most 12 times a boot). Per device: address, hub and port
(`otg-port` when it is on the OTG port itself), speed (LS/FS/HS), `split` when the DWC2 driver runs
it through split transactions, VID:PID, device class, EP0 size; per interface: class/subclass/protocol
and endpoints (address, type, max packet, interval); then what the kernel did (`keyboard ACCEPTED`,
`HID skipped: not a boot keyboard`, `HID slots full`, `CONFIG REJECTED: ...`, `mounted`, `removed`). A
device that was read but is not mounted after 4 s gets a `NOT MOUNTED` line (TinyUSB drops a failed
enumeration silently and this build has no TinyUSB debug log). The last line counts attach and remove
events.

Read it from a Mac with the normal kernel running (no adapter): `usb_kernel.py <port> usblog`
(`USBLOG <n>` returns the file in numbered pieces, newlines as `|`).

Support facts (checked in the sources, 2026-11-10): TinyUSB 0.21's DWC2 host driver does split
transactions (`hcd_dwc2.c`, `split_en` set for any non-high-speed device when the root port runs at
high speed), so full and low-speed devices behind a high-speed hub are supported. The HID glue accepts
any interface that is a boot keyboard (subclass 1, protocol 1), on any interface number, and ignores
mouse and other interfaces; report IDs on another interface do not matter.

## The fault record (FAULT.TXT)

When the OS program faults (abort, undefined instruction, missed heartbeat, or a nonzero exit), the kernel
saves `/OS/SAVES/KERNEL/FAULT.TXT` before it restarts the program or falls back to DIAG.ELF. Line 1 has the
program, the kind, the PC, the fault status and address registers, how long the program ran, its system calls
and presents, and how many times it was started. Each following `>` line is one of the program's last four
log lines (those OS log lines are otherwise only on the serial port). Read it on a Mac with the card in a
reader. A new fault replaces the file, so read it after the first crash at the TV.
