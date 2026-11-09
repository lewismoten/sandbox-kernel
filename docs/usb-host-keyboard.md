# USB host keyboard: plan after ID detection

Slice 1 (done): read the OTG ID pin once at boot and show `USB: OTG (KEYBOARD NEXT)`
when the ID pin is grounded and no Mac enumerates. Nothing enters host mode yet.

## Trigger (Lewis's choice)
Host mode only when ID is grounded (OTG adapter) and no Mac data link. Otherwise the
gadget stays as is. `core/usbrole.c` holds the debounced decision (tests/usbrole.c).

## Register facts
- GOTGCTL at 0x20980000 (base 0x20980000, GUSBCFG at +0x0C as used in drivers/usb.c).
- GOTGCTL.ConIDSts = bit 16, 1 = B-device, 0 = A-device: Linux `drivers/usb/dwc2/hw.h`,
  `GOTGCTL_CONID_B BIT(16)`. Checked 2026-11-09.
- GUSBCFG.ForceHstMode = bit 29, ForceDevMode = bit 30: same file.
- Forced mode hides the real pin: Linux `dwc2_clear_force_mode()` comment says the
  connector ID status "is affected by the force mode" and allows up to 100 ms of IDDIG
  filter delay after clearing the bits. So read it with both force bits clear, after a
  wait, then force a role. Slice 1 does this once at boot. VERIFY on hardware.
- VERIFY: whether the pin reads right before TinyUSB's core reset and PHY setup; the
  Zero W board routes the OTG connector ID pin to the core (believed, not checked).
- Live hot-plug sampling needs a brief un-force window (or the A/B-device change
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
