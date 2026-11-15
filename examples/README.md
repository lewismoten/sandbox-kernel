# Examples

Small OS programs, each built with the SDK only. Copy one to start your own OS.

- [hello/](hello/README.md): "Hello, world!" and a box you move with the arrow keys.
- [clock/](clock/): uptime as MM:SS from `sbx_clock_ns`, waiting with `sbx_sleep_until`. Esc exits.
- [cectest/](cectest/): a test screen for the TV remote: the last received CEC frames (raw, decoded, what the kernel maps them to), this Pi's CEC addresses and active-source state, and the last 8 distinct events with counts. Hold Select 3 s, or Esc on a USB keyboard, exits. Needs the kernel's `SBX_IOC_INPUT_CEC` (shows only kernel keys without it).

`make examples` builds all; `make test` checks they link and obey the loader rules.
