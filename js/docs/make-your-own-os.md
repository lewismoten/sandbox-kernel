# Make your own OS in JavaScript

You will end up with a Pi Zero W that boots your own program from the card: a home screen, apps, settings that survive a restart. You write JavaScript only. [`examples/TINYOS/`](../examples/TINYOS/) is a finished, small example (a home screen, a Files viewer, Settings, an About page, a startup chime and an e-ink clock; about 170 lines) that you copy and change.

> **Status: host-tested only.** Everything here runs on your computer through `jsrun`, the same runtime over stdio with models of the kernel (files, display, audio, e-ink, power). TINYOS and this guide have **not** been qualified on a real Pi Zero W. Untested on hardware: booting `JS.ELF` from AUTORUN, the real key repeat and USB keyboard timing, the chime (HDMI audio path), the e-ink panel and its refresh limits, a save surviving a real power cut, `putapp` of this folder, and the speed of the home screen on the ARM1176. Treat a first boot as an experiment, and keep a way to recover (the kernel's recovery console and `usb_kernel.py`, [os-authors.md](../../docs/os-authors.md) sections 2 and 6).

## 1. What you need

- Python 3 (the checker and the tests), a C compiler (`cc`) for the host runner, and this repository.
- To build `JS.ELF` yourself: the Arm GNU Toolchain (`arm-none-eabi-gcc`; the Makefile defaults to `/Applications/ArmGNUToolchain/15.3.rel2/arm-none-eabi/bin/arm-none-eabi-`, override with `TOOLCHAIN=`). There is no prebuilt `JS.ELF` published yet (a slice 6 item). You can write and test your whole OS on the host without the toolchain, and take a `JS.ELF` built by someone else when there is one.
- A Pi Zero W running sandbox-kernel, and either a card reader or the USB data cable.

## 2. Build the runner and the ELF

```sh
make -C js host    # build/host/jsrun: your computer plays the kernel
make -C js         # build/JS.ELF (about 826 KB; the card's image quota is 8 MiB)
```

## 3. Lay out the card

```
/OS/JS.ELF                      the runtime
/OS/AUTORUN.TXT                 line 1: JS.ELF    line 2: APPS/MYOS
/OS/APPS/MYOS/APP.JSON          the manifest: name, api, capabilities
/OS/APPS/MYOS/MAIN.JS           your entry script
/OS/SAVES/MYOS/                 made for you; your app may write here (fs.write.saves)
```

For TINYOS, copy the folder `js/examples/TINYOS` to `/OS/APPS/TINYOS` and write `AUTORUN.TXT` as two lines, `JS.ELF` and `APPS/TINYOS`. The kernel reads line 1 and starts `JS.ELF`; `JS.ELF` reads line 2. If line 2 is wrong or your app will not start, the built-in picker starts and shows the reason, so a mistake does not leave you with a dark screen.

Rules that shape an OS written this way (all from [apps.md](apps.md)):

- An app reads its own folder and its save folder only, never a sibling's files. With the `apps.launch` capability it may list the other apps' manifests (`apps.list()`) and start one (`apps.launch(id, {args})`); the other app brings you back with `apps.exit()` ([apps.md](apps.md), gap G5). TINYOS does both: its own "programs" are files in its folder (`ABOUT.JS`, `FILES.JS`, `SETTINGS.JS`; the first line `//! Name` is what home shows, loaded with `fs.readFile` and `new Function`), and below them home lists every other app on the card. A launched app only returns home if it calls `apps.exit()` (for example on Esc); one that does not is left with F10, which goes to the picker.
- Writes are whole files in `/sd/OS/SAVES/<id>/`, names 8.3 (`SETTINGS.CFG`), at most 256 KiB per app. The kernel has no subfolders, so keep your folder flat; the USB tool only installs flat folders.
- Ask for the capabilities you use in `APP.JSON` and nothing else. TINYOS asks for `display`, `input`, `audio`, `eink`, `fs.read`, `fs.write.saves`, `apps.launch`.
- Give back what you hold (`panel.release()`, `audio.close()`) before `sys.exit`; the runtime reports what is left open.

## 4. Check it

```sh
python3 js/tools/app_check.py js/examples/TINYOS     # the app: manifest, entry, folder name
python3 js/tools/app_check.py --card /Volumes/SD     # every app on a mounted card and the one AUTORUN.TXT names
```

It applies the same rules the runtime does, so a card that passes will not be refused for its manifest.

## 5. Debug on the host first

```sh
mkdir -p /tmp/card/OS/APPS && cp -R js/examples/TINYOS /tmp/card/OS/APPS/
printf 'JS.ELF\nAPPS/TINYOS\n' > /tmp/card/OS/AUTORUN.TXT
js/build/host/jsrun /tmp/card x --autorun --vclock --ms 3000 \
  --keys 300:108:1,320:108:0,400:28:1,420:28:0 --shot /tmp/home.shot
python3 js/tools/jsshot.py /tmp/home.shot /tmp/home.png
```

- `--autorun` boots the way `JS.ELF` does, from `AUTORUN.TXT`. Without it, `jsrun <card> /sd/OS/APPS/MYOS` starts one app.
- `--keys ms:code:value,...` is a key script: Linux key codes (up 103, down 108, left 105, right 106, Enter 28, Esc 1, Q 16), value 1 down and 0 up. `--vclock` makes time virtual, so a minute of clock runs instantly and every run is repeatable.
- `console.log` lines come out prefixed `JS `. A thrown error prints its stack with file and line.
- `--shot` saves the final framebuffer; `jsshot.py` makes a PNG. `--eink-dump FILE`, `--audio-dump FILE`, `--no-eink`, `--no-audio` and `--devstat` show or remove the devices.
- Write a test like [`tests/os_run.py`](../tests/os_run.py): boot, press keys, compare a framebuffer CRC, restart, read the save file. `make -C js test` runs it with everything else.

## 6. Put it on the card

Card reader: copy `JS.ELF` to `/OS/`, your app folder to `/OS/APPS/`, and `AUTORUN.TXT` to `/OS/`; eject properly. Names are case-blind.

USB (the Pi's data port; [os-authors.md](../../docs/os-authors.md) section 6):

```sh
scripts/usb_kernel.py <port> put js/build/JS.ELF
scripts/usb_kernel.py <port> putapp js/examples/TINYOS TINYOS
scripts/usb_kernel.py <port> run JS.ELF          # or reboot
```

`AUTORUN.TXT` is a card file; the simplest way is the card reader (`put FILE --name AUTORUN.TXT` should also work, untested).

## 7. Boot and look

Power on. Expect the kernel's boot screens, then TinyOS's home list and a four-note chime; Up and Down choose, Enter opens a page or starts an app, Esc goes back (inside an app that calls `apps.exit()` on Esc, back home), Q quits to the picker. With an e-ink panel fitted it shows uptime as `HH:MM` and refreshes about every 30 seconds. `usb_kernel.py <port> tail` shows the log and `fbshot` / `einkshot` save what the screens show. A bad edit lands in the picker with the reason on screen; fix the file and reboot.

## 8. Make it yours

Change `THEMES` and the program files in `TINYOS`, or start from `MAIN.JS` alone: a `view` is `{ rows(), sel, key(name) }` and the shell draws it. For a page-shaped UI use the DOM-lite API; for pixels, `screen.lease()` ([apps.md](apps.md)).
