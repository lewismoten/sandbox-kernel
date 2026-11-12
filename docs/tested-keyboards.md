# Tested keyboards

Keyboards tried on real hardware with the host role (`USB_ROLE=host`, see
[usb-host-keyboard.md](usb-host-keyboard.md)). Only what a log or a person's test confirmed is
marked as working; everything else says "not tested". The trackpad and media keys are not
supported yet: the kernel reads boot keyboards only.

| Keyboard                                         | Receiver / wired | USB VID:PID  | Speed        | Through a hub?                     | Kernel                                                           | What works                                                                                                                                                              | Notes                                                                                                                   | Date       | Tested by        |
| ------------------------------------------------ | ---------------- | ------------ | ------------ | ---------------------------------- | ---------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- | ---------- | ---------------- |
| Rii mini X2 RT-MWK01                             | 2.4 GHz receiver | 1997:2433    | Full         | Yes, hub 1A40:0101 (split)         | 139d140 (feature/adv-fixes-1111), host companion gen 31, on PI-2 | Keys, menus, games, high-score name entry on a desk monitor and a living-room TV. Arrows, F-keys, LEDs: not tested separately. Trackpad, media keys: not supported yet. | Interface 0 is a boot keyboard (accepted); interface 1 (mouse protocol: trackpad, system and media control) is skipped. | 2026-11-11 | Lewis            |
| Unknown keyboard (second device on the same hub) | Wired or unknown | 1A2C:2124    | Low          | Yes, hub 1A40:0101, port 4 (split) | same                                                             | Enumerated and mounted; interface 0 accepted as a boot keyboard; first report received. Keys, arrows, F-keys, LEDs: not tested.                                         | Interface 1 (report protocol) is skipped. Make and model not recorded.                                                  | 2026-11-11 | Lewis (log only) |
| Rii mini X1                                      | 2.4 GHz receiver | not recorded | not recorded | not recorded                       | not recorded                                                     | Works, per a person's test. Which keys were tried: not recorded. Trackpad, media keys: not supported yet.                                                               | Add the USB.TXT lines to fill in the IDs.                                                                               | 2026-11-12 | Lewis            |
| Rii RK907                                        | Wired            | not recorded | not recorded | not recorded                       | not recorded                                                     | Works, per a person's test. Which keys were tried: not recorded.                                                                                                        | Add the USB.TXT lines to fill in the IDs.                                                                               | 2026-11-12 | Lewis            |

Sources: the Rii receiver's descriptors from the Mac (2026-11-10) and the `USB.TXT` from the
2026-11-11 TV test.

## Adding a row

Run `usb_kernel.py <port> logs` and copy that keyboard's `USB.TXT` lines (the `Dn ... VID:PID`,
`hidN ... ACCEPTED` or `skipped` and `first report` lines), then note the kernel version, hub use
and what you tried (keys, arrows, F-keys, LEDs). Write "not tested" for the rest.
