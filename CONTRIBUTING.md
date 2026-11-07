# Contributing

New kernel code, SDK code, and documentation use the [MIT license](LICENSE).
Keep the kernel generic: game formats, gameplay, UI policy, decoding, and music
synthesis belong in the OS program. Drivers and hardware access belong in the
kernel. Changes must preserve isolation and documented ABI compatibility.

## Clean-room rule

Do not copy, translate, closely paraphrase, or recreate the structure of Circle
GPL code in this repository. Do not import Circle headers, wrappers, build files,
fonts, or binaries. A syscall boundary or a new filename does not relicense code.
For this repository, obtain even permissive dependencies directly from their
upstreams rather than extracting them from Circle.

The separate prototype may supply hardware observations only: wiring, controller
behavior, transfer framing, and media layout. Record those facts in prose,
separate from the implementation. Derive new drivers from Broadcom/Raspberry Pi
documentation, Arm manuals, and independently obtained permissive sources.
Do not use the prototype's classes, algorithms, or control flow as a coding template.
This is the project's development rule, not a claim of a formally audited legal
clean-room process.

For each driver or dependency, record:

- The hardware documents and sections used, including revision and errata.
- Upstream URL, exact commit or release, file hashes, author credits, and the
  actual licenses of every retained file and required dependency.
- Which code was written here and which code was adapted, with all upstream
  notices retained. Update NOTICE.md and the dependency inventory.
- Board/revision, firmware version, configuration, test steps, measured results,
  and remaining limitations. A successful build does not establish hardware support.

TinyUSB, FatFs, lwIP, and Waveshare are planned sources, not bundled dependencies.
Audit file-level terms before importing anything. Keep boot/radio firmware and
its matching redistribution notices separate from MIT source. Do not silently
substitute GPL, noncommercial, or unverified code when a driver is difficult.

## Review and validation

Document ABI numbers, structures, errors, capabilities, and compatibility effects
before implementing a public interface. Use fixed-width fields and explicit
lengths; never expose kernel pointers, register addresses, or DMA descriptors.
Test malformed images and user pointers, timeout recovery, resource limits, and
power interruption where relevant. Follow the acceptance gates in
[docs/milestones.md](docs/milestones.md). Report emulator, host, and physical-board
results separately. Use spare media for destructive power-loss tests.

Contributions must be reviewable as ordinary file changes. Do not commit, stage,
change branches, or otherwise write Git metadata unless the maintainer requests it.
