# sandbox-kernel

A small, reusable bare-metal Raspberry Pi kernel under the MIT license.
**Status: design.** This repository contains specifications, not a bootable
kernel, drivers, SDK, or tested hardware support.

The first target is Lewis's Raspberry Pi Zero W v1.1 (BCM2835, ARM1176).
Later ports target Pi 1, 2, 3, 4, and 5 through separate board backends.
The kernel owns hardware and protects itself from a separately loaded,
user-mode OS program. Game-Maker OS is the first intended program, maintained
separately in [lewismoten/gamemaker-os](https://github.com/lewismoten/gamemaker-os).
Other OS programs use the same generic interface.

Goals:

- A familiar, versioned POSIX-style system-call subset and device paths.
- MMU isolation, bounded driver operations, watchdog recovery, and visible errors.
- HDMI graphics, e-ink status, storage, input, and eventually audio and wireless.
- Verified USB updates with A/B kernel and OS slots and automatic fallback.
- A small image, measured in MB rather than GB; size targets await implementation.
- Original MIT code and audited permissive dependencies, developed independently
  of Circle. Vendor boot and radio firmware retain separate licenses.

Read the [design](docs/design.md), [milestones and acceptance tests](docs/milestones.md),
[credits](NOTICE.md), and [contribution rules](CONTRIBUTING.md).
The design selects route B from Lewis's November 6, 2026 license research.
Earlier Linux and Circle plans do not define this kernel's implementation.
There are no build or flash commands yet. Nothing here has been tested on a Pi.
