# E-ink API (ABI 1.9)

The Pi Zero W's Waveshare 2.13" V4 panel (SSD1680, black and white) is owned by the kernel.
This page is the API that lets the running OS change what it shows, and the rules that keep
the panel and the kernel's own screens safe. Source: `core/eink.c` (rules, no hardware),
`core/render.c` (drawing), `drivers/panel.c` (SPI), `core/sys.c` (the device).

Introduced in **ABI 1.9**, after saves (1.7) and audio (1.8); the current
kernel and SDK report **1.11**. The OS API and refresh-care rules are
hardware-untested; M0 board qualification covers the kernel boot/status screens.

## The screen

250 by 122 pixels, landscape (the panel's native 122x250 is mapped by `render.c`). Every
bitmap in this API uses one layout, the same as the kernel's blit masks (`struct logo`):

- row-major, top row first, left pixel first;
- each row is `ceil(width / 8)` bytes (a page row is 32 bytes, a mark row 5 bytes);
- **bit 7 of a byte is the leftmost pixel of its eight; a 1 bit is black, 0 is white**;
- the padding bits at the end of a row (the last 6 of a page row's 32 bytes) are ignored.

So pixel (x, y) of a page is black when `page[y*32 + x/8] & (0x80 >> (x%8))`.
A page is 32 x 122 = **3,904 bytes**. A mark is 40x40 = **200 bytes**.

## Two levels

1. **Title.** The OS sets its own name (1 to 16 characters) and optionally a 40x40 mark. They
   replace `SANDBOX KERNEL` and the chip mark on the status screen while the OS runs. The line
   under the name is the OS's **build line**: free text of up to 16 characters (what fits the
   line at 2x), such as a hash (`3A9F2C1`), a version (`V0.4.2`), a date (`2026-11-09`) or a mix
   (`V0.4 3A9F2C1`). With `build_bytes` 0 it is blank. The kernel's own build ID is never shown
   under an OS name; it returns with the kernel's screens, USB INFO and after a clear or reclaim.
   `UP:`, `USB:` and `ERR:` stay the kernel's. The build line uses the same characters as the
   name; lowercase shows as capitals and anything else shows as `?`. More than 16 bytes is
   `EINVAL` in the record (the SDK wrapper clips for you).
   Name characters are the kernel font's: `A-Z`, `0-9`, space and `-:./()=,_+!?';%`.
   Lowercase letters are shown as capitals. Anything else is `EINVAL`.
2. **Page.** The OS submits a whole 250x122 bitmap, shown **instead of** the status screen.
   This is what an animation (a desk buddy such as Scoot) uses: one page per frame.

A page wins over a title (the page covers the whole screen). Clearing returns to the kernel's
status screen.

## The device: `/dev/eink`

Open `/dev/eink` **write-only** (`SBX_O_WRONLY`); one handle at a time (`EBUSY` otherwise),
`ENODEV` (on open and on every call) before the panel exists, and again if the panel driver gives up (a BUSY timeout disables it). It needs no mmap.

| Operation | How | Record or data | Result |
| --- | --- | --- | --- |
| Query | `ioctl SBX_IOC_EINK_QUERY` (class 4, op 0) | `sbx_eink_info`, 80 bytes, all fields zero on entry | the sizes and limits below |
| Set the title | `ioctl SBX_IOC_EINK_TITLE` (4, 1) | `sbx_eink_title_rec`, 272 bytes: `flags` (bit 0 = a mark follows), `name_bytes` (1-16), `name[16]`, `mark[200]`, `build_bytes` (0-16), `build[16]` | 0, `EINVAL`, `EAGAIN` |
| Clear | `ioctl SBX_IOC_EINK_CLEAR` (4, 2) | `sbx_header` only (16 bytes) | the kernel's title and status screen return |
| Submit a page | `write` of **exactly 3,904 bytes** | the bitmap | the count, `EINVAL` for any other size, `EAGAIN` |

`flags` and `reserved` header words must be zero, as everywhere in the ABI. `name` bytes
after `name_bytes`, `build` bytes after `build_bytes`, and `mark` when bit 0 is clear, must be zero.
A write is all or nothing: no partial pages, no append. Closing the handle does **not** clear
the title or page (the OS may open the device only when it wants a change); the kernel clears
them when the program ends, however it ends.

## Panel care (limits the kernel enforces)

E-ink flashes and ghosts on a full refresh, and the panel ages with refreshes. The OS has no
way to ask for a full refresh; the kernel decides.

| Limit | Value | Where it comes from |
| --- | --- | --- |
| Pages and titles redraw by **partial** refresh (about 0.3 s, no flash) | always | Waveshare 2.13" V4 manual: partial refresh is for quick updates |
| Minimum interval between accepted updates | **1 s**; sooner is `EAGAIN` (nothing changes) | VERIFY: a safe round number, not a datasheet figure |
| At most **30** accepted updates in any 60 s | `EAGAIN` beyond | VERIFY: guess; stops a loop from running the panel flat out |
| **Ghost-clearing full refresh** after every **50** partial refreshes, and never sooner than **180 s** after the last full one | done by the kernel on the next redraw | Waveshare wiki and manual, 2.13" V4: "refresh interval of at least 180 s; refresh at least once every 24 hours; after several partial refreshes, do a full refresh". VERIFY the 50 against the SSD1680 panel guidance and the real panel |
| After each finished refresh the driver sends **deep sleep mode 1** (command 0x10, data 1); the next refresh starts with a hardware reset | always | Waveshare 2.13" V4 demo sleeps the controller after display. The wake sequence also resends the internal temperature sensor choice (0x18, 0x80), which a reset forgets. Hardware-untested |
| A full refresh at least once every **24 h** | kernel: its own deadline of **23 h** after the last full refresh, kept even when the screen never changes (a static page is redrawn in full) | same source; one hour of margin |
| The boot refresh | counts as a full refresh: the 180 s gap and the 23 h deadline run from the boot full-refresh command trigger | the next refresh can never land inside the gap |

`EAGAIN` is -11. The rejected submission is dropped, not queued: wait and try again. A
sensible animation is at most one frame a second.

## The kernel always wins

The rule is simple on purpose:

1. **End of the program.** When the OS exits, faults, is stopped or is replaced (every path
   goes through `sys_reset`), the title and page are cleared. The next panel redraw is the
   kernel's own status screen (with the `ERR:` line for a fault).
2. **A new kernel error while a page is up.** The panel drops the page **at its next
   update** and shows the kernel status screen with the error. The page comes back only
   when the OS submits another page (so at least 1 s later), so an error is never hidden for
   more than one update.
3. **The error mark.** From then on, any page shows a **kernel indicator**: a solid 8x8 black
   square in the top-right corner (x 242-249, y 0-7), drawn over the OS bitmap, for as long
   as `ERR:` is not `none`. Pages should leave that corner white (the API suggests it). The
   title screen needs no mark: it already shows `ERR:`.

"New" means the error text changed since the panel last drew it. Note that the kernel's last
error is set by real USB failures, including storage-not-ready on PUTF/RUN/PUT2.
Flow-control refusals (for example PUTF while an OS is running, or no INFO session)
do not set it. Screen/file-read refusals also leave the sticky error unchanged; see
[the error table](os-authors.md#reading-the-e-ink-panel).

## Review pictures

`scripts/render_screen.py OUTDIR` also renders the title screen and a test page (the files
`eink-title.png`, `eink-page.png`, `eink-page-error.png`).

## Not done yet

- Hardware check: the ghost-clearing full refresh after boot uses the boot sequence's full
  path (`panel.c`, phase 7 and 8) and has only been run on the host. VERIFY on the board.
- No grayscale, no fast-refresh mode, and no per-region update for the OS: it always sends a whole page. Inside the driver a partial refresh writes only the rectangle of RAM that changed (0x44/0x45 window, 0x4e/0x4f counters), to both 0x24 and 0x26, so 0x26 always equals the glass; the panel still refreshes whole, but only changed pixels are driven.
