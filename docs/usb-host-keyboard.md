# USB host keyboard: plan after ID detection

Slice 1 (done): read the OTG ID pin once at boot, latch the result, and show
`USB: OTG (KEYBOARD NEXT)` when that latched ID was grounded and no Mac has enumerated for
500 ms. The role intent is not re-sampled (hot-plug is not implemented yet; slice 2). The
kernel does not start host mode in this slice.

## Trigger (Lewis's choice)
Planned: host mode only when ID is grounded (OTG adapter) and no Mac data link; today the
kernel only reports that state. Otherwise the gadget stays as is. `core/usbrole.c` holds the debounced decision (tests/usbrole.c).

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

Guarantee, precisely: the kernel code never sets ForceHstMode and never writes HPRT0 (so it never
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
  "no computer enumerated" half of the decision is live (500 ms debounce, `usbrole_step`).
  Live hot-plug sampling needs a brief un-force window (or the A/B-device change
  interrupt); that disturbs a gadget link, so it is slice 2.

## Next slices
1. Host switch: clear ForceDevMode, set ForceHstMode, wait ~25 ms for CurMod (GINTSTS bit 0),
   power the port (HPRT.PrtPwr, bit 12), core soft reset first. Return to device on
   unplug or when a Mac link appears (reboot-free path, or simply reboot at first).
2. Root port: wait for HPRT.PrtConnDet, reset the port (HPRT.PrtRst, 10 to 20 ms), read
   speed, set HFIR/HCFG for the PHY clock. Host channels in slave (PIO) mode, matching
   the PIO DWC2 backend already in use (no USB DMA, include/tusb_config.h).
3. Enumeration: control transfers on channel 0 (GET_DESCRIPTOR, SET_ADDRESS,
   SET_CONFIGURATION), split into small testable state machines.
4. Hub: unpowered hub means bus power only: check the hub descriptor current,
   SET_FEATURE PORT_POWER, poll port status, reset, enumerate the child. Low-speed
   keyboards behind a high-speed hub need split transactions (the Zero's core is
   high-speed): the hard part; test with the keyboard direct first.
5. HID boot keyboard: SET_PROTOCOL(0), SET_IDLE, poll the interrupt-IN endpoint every
   bInterval ms, 8-byte boot report to key events into core/input.c.

## Code to model on
Not GPL (no Linux/FreeBSD dwc code copied). Candidates: CSUD, the USB driver in Baking
Pi (Alex Chadwick, MIT), or Plan 9's usb stack (MIT-style license; VERIFY). Credit the
source in NOTICE.md when any code is adapted. TinyUSB (already used, MIT) has a DWC2 host
driver (`portable/synopsys/dwc2/hcd_dwc2.c`) and HID host class; evaluating it first is
cheapest, though the design file asks to audit its BSP needs.
