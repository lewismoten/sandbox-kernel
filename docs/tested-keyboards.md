# Keyboard, receiver and hub compatibility

This is an evidence inventory, not a hardware certification list. The current
host stack still needs board qualification. Native fixtures exercise descriptors,
reports and recovery; they do not demonstrate that a particular retail device
works on the Pi. The Rii model and hub below were requested for this inventory,
but this checkout contains no corresponding hardware capture or keyboard research
notes. Unknown values stay unknown rather than being inferred from a model name.

See [host installation and USB logging](usb-host-keyboard.md) for the companion
setup and [bounded USB waits](usb-bounded-waits.md) for controller timeout behavior.

## Devices and evidence

`mps` is an endpoint's maximum packet size, not the observed report length.
An EP0 size describes control transfers, not keyboard reports.

| Keyboard, receiver or adapter | VID:PID | Enumeration and report length | What works / evidence | Known problems and gaps |
| --- | --- | --- | --- | --- |
| Rii mini X2 RT-MWK01 wireless receiver | Unknown; capture the receiver's device line | Boot capability, interfaces, speed and actual report length unrecorded. Do not assign an ID from the model name. | Named hardware to qualify; `tests/usbkbd.c` describes its simulation as “Rii-style,” not a capture from this model. | No model-specific stall, missing-key or media-key results available. Mouse/touchpad and consumer-control interfaces are skipped by the current keyboard glue. |
| Hub identified in the hardware request | `1A40:0101` | Hub descriptor, speed, ports and interrupt bitmap length unrecorded; keyboard boot protocol/report length does not apply to the hub itself. | Listed in the [reported hardware table](usb-host-keyboard.md#reported-hardware); no matching hardware capture or fixture in this checkout. | Interoperability, power and downstream enumeration unqualified. Generic control and hub-poll recovery tests do not establish results for this hub. |
| Keyboard/receiver used in multi-keyboard and log fixtures; retail model unknown | `1997:2433` | Synthetic device line: FS behind hub1 port2, split transactions, device class 0/0/0, EP0 32. Glue fixture supplies an accepted boot keyboard and skips a mouse; keyboard reports are 8 bytes. | `tests/usblog.c` checks formatting; `tests/usbkbd.c` checks two keyboards, shared input and release on removal. | No captured descriptors or physical results. Speed and EP0 values are fixture inputs. Missing keys and stalls on the real device remain unknown; consumer media keys are unsupported. |
| Rii-style composite keyboard simulation; retail model unknown | `1A2C:000C` | Callback fixture accepts HID index 1 as a boot keyboard and skips a non-keyboard interface. Injected reports are 8 bytes; no real device/configuration descriptor supplied. | `tests/usbkbd.c`: Up, Space, Enter, Shift+A, release, unplug/replug, and ignoring short/rollover reports. | A synthetic identity, not a verified RT-MWK01 identification. Real enumeration, report length and device-specific failures unknown. |
| Hub in the USB log formatter fixture; retail model unknown | `05E3:0608` | Synthetic root-connected HS hub, class 9/0/1, EP0 64. Boot protocol and keyboard report length do not apply. | `tests/usblog.c` checks the device-line text. | Formatting only; no hardware evidence for ports, downstream transfers, stalls or power. |
| Passive micro-USB OTG adapters, including grounded-ID and absent/floating-ID cases | None (passive adapters do not enumerate) | Grounded ID at cold boot selects an installed companion after debounce; no HID protocol or reports. | `tests/test_usbrole.py` and `tests/usbrole.c` cover role decisions, timing and link priority. The host guide records a boot-window correction from the 2026-11-10 hardware test. | No adapter brand/model or electrical qualification recorded. Attach before power-on; live role switching is not implemented. An adapter containing a hub needs a separate hub entry and capture. |
| Unnamed wired USB boot keyboards | Unknown | Generic class 3/subclass 1/protocol 1 keyboard; 8-byte boot layout expected. No named wired model or capture in the checked-in notes. | Generic descriptor/parser/glue tests below apply to the USB interface regardless of cable or radio transport. | No per-model hardware pass. Report-only/NKRO layouts and unmapped usages need additional support. |
| Unnamed wireless keyboards and 2.4 GHz composite dongles | Unknown | Generic fixtures place a boot keyboard before or after mouse/consumer interfaces. Actual receiver identity and radio behavior unknown. | `tests/hidboot.c` tests keyboard interfaces 0 and 1, interrupt IN mps 8, intervals 10 and 8; mouse/consumer interfaces are ignored. | No additional named wireless model in this checkout. Pairing, wake and RF reliability are untested; separate media interfaces are skipped. |

The other numeric identities passed to `usbkbd_attached` in `tests/usbkbd.c`
are arbitrary fake devices for slot/union tests, not additional retail keyboards.
No checked-in research note establishes further wired or wireless models.
When those notes become available, add each model with its own evidence rather
than treating these generic rows as a hardware pass.

## Shared behavior and limitations

The runtime uses TinyUSB enumeration, accepts boot-keyboard interfaces in boot
mode, and requests boot protocol before the mount callback. There is no VID:PID
allowlist: matching the interface and report format matters. One hub, four
downstream devices and four accepted keyboard interfaces are configured, with
space for eight HID interfaces including skipped mouse/consumer interfaces.

The parser consumes an 8-byte boot report (modifiers, reserved byte, six usages).
The glue's first received report selects a leading report-ID workaround when its
length is exactly 9; it then strips that byte from subsequent reports longer
than 8. Other long reports use the first 8 bytes. This is a heuristic, not a
general HID report-descriptor parser. Reports shorter than 8 are ignored; rollover
reports preserve the previous held keys until a valid report or unplug. Fixtures
exercise 7-, 8-, 9- and 10-byte reports. HID IN endpoints larger than the 64-byte
receive buffer reject the entire configuration, even on a skipped interface.

Letters, digits, punctuation, F1–F12, navigation, keypad keys and eight modifiers
map to Linux key codes (`core/hidboot.c`, `tests/hidboot.c`). Usages outside
0x04–0x63 are unmapped, including the non-US key at 0x64; consumer/media keys
on separate interfaces are not decoded. There is no general NKRO/report-mode
support. A ready keyboard can therefore still have missing keys. Held keys are
combined across keyboards; unplug releases only keys no longer held elsewhere.

Control failures retry after 20/40/80 ms; STALL is logged immediately. SET_IDLE
and SET_PROTOCOL can legitimately stall, while other failed enumeration requests
can prevent mounting. Optional string failures skip the remaining strings.
Downstream/root enumeration gets at most two reset retries after 200/400 ms;
hub interrupt polling has its own bounded refusal retries. Persistent keyboard
poll submission refusal removes that keyboard and releases its keys. A terminal
DWC2 timeout disables USB. `tests/test_usbhost_retry.py` exercises recovery with
a fake controller when pinned TinyUSB sources are present; these are shared
failure paths, not recorded defects of any specific device above.

## Add a keyboard, receiver, hub or adapter

1. Record the exact label/model, wired or wireless transport, receiver identity,
   Pi revision, kernel build/generation, adapter and hub topology, hub port and
   power arrangement. Test direct connection and through the hub separately.
   Install the host companion using the [host guide](usb-host-keyboard.md),
   then cold boot with the adapter attached.
2. Record boot handoff, `USB host up`, `keyboard ready`, and actual key behavior.
   Exercise letters, digits, modifiers, arrows, Enter, function keys, keypad,
   non-US keys and media keys that exist on the device. Test multiple held keys,
   release, unplug/replug and repeated cold boots. Record which keys fail and
   whether the failure occurs directly, behind the hub, or both. Save the matching
   OS build and any available UART output containing `key NAME` press lines;
   USB.TXT does not contain a full key/report trace.
3. Allow the log to become quiet for at least 1.5 s with storage ready and no OS
   running so it can save. Power off, remove the adapter, and reconnect the Mac
   data cable to boot the normal device kernel. Read the previous host record:

   ```sh
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX info
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX lastboot
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX usblog
   python3 scripts/usb_kernel.py /dev/cu.usbmodemXXXX logs --out keyboard-run-01
   ```

   Use a fresh output directory per run. `logs` collects saved kernel records;
   there is no CDC link while the host kernel runs. USB.TXT is bounded to an
   8192-byte buffer, saved at most 12 times per boot, and an older record may
   remain if saving was deferred or failed. Check that its topology matches the
   run; retrieve it with a card reader if needed.
4. Send the complete saved USB.TXT and BOOT.TXT with the observations above.
   Preserve these USB.TXT lines in particular (addresses and HID indices vary):

   - Device lines such as `D3 hub1 port2 FS split 1997:2433 class 0/0/0 ep0 32`,
     and the upstream hub's device line. These establish VID:PID and topology.
   - Every `D… I… class …` and endpoint line (`ep`, `mps`, `every`), plus
     `hid… = interface …, boot protocol …, mode …`, `keyboard ACCEPTED`, skipped
     interfaces and `CONFIG REJECTED` messages.
   - Report-length notes (currently truncated to `first report l8` or
     `first report l9`, and e.g. `short report i7`), `report ID byte stripped`,
     `mounted`, `enumeration complete`, `removed` and `keyboard poll failed`.
   - All `CTRL` failures/recoveries/stalls, hub `enum` and `poll` retry outcomes,
     `NOT MOUNTED`, `USB stall absorbed`, DWC2 timeout messages and event counts.

   A descriptor endpoint's mps alone is insufficient to fill in report length.
   `keyboard ACCEPTED` alone does not prove complete device enumeration or key
   delivery. Preserve failure lines rather than summarizing them as “stalled.”
5. Add a table row with the captured identity, interface/boot mode, actual first
   report length, tested keys, topology, failures and evidence date/build. Keep
   unknowns explicit. Add a sanitized fixture or a separate sourced docs table
   for any new VID:PID; `tests/test_docs_keyboards.py` checks that the identities
   in this inventory have that independent reference. Run `make test`. A new
   VID:PID alone requires no driver change when the existing boot layout works.
