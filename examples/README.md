# Examples

Small OS programs, each built with the SDK only. Copy one to start your own OS.

- [hello/](hello/README.md): "Hello, world!" and a box you move with the arrow keys.
- [clock/](clock/): uptime as MM:SS from `sbx_clock_ns`, waiting with `sbx_sleep_until`. Esc exits.

`make examples` builds both; `make test` checks they link and obey the loader rules.
