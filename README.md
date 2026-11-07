# sandbox-kernel

A small, reusable bare-metal Raspberry Pi kernel under the MIT license.
**Status: design.** This repository contains specifications. The kernel itself is not written yet.

The first target is the Raspberry Pi Zero W v1.1 (BCM2835, ARM1176).
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
The kernel is MIT-licensed and written independently; earlier Linux and Circle plans do not define it.
There are no build or flash commands yet. A separate Circle-based prototype has already booted on a
Zero W with the e-ink status display and USB updates working; what it taught us is folded into the
[milestones](docs/milestones.md).
