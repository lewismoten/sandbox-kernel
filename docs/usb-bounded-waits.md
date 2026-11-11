# USB latency bounds

The TinyUSB 0.21.0 sources remain immutable. `scripts/usb_bounded_overlay.py`
verifies every file against the pinned SHA256 manifest and applies
`patches/tinyusb-0.21-bounded.patch` to a private build copy. Both roles compile
that port; host enumeration/retry changes remain in the existing host overlay.
The upstream MIT notices remain in the generated sources. No ABI changes.

Hardware waits use the BCM2835 free-running microsecond counter, including
inside IRQs or critical sections. Unsigned subtraction handles timer wrap.
The deadline bounds software polling of accessible MMIO; it cannot recover
from a bus read that itself never completes or a stopped system timer.

| Poll / location | Deadline / termination | Failure behavior |
| --- | --- | --- |
| `dwc2_common.c`: AHB idle before/after reset | 20 ms each (`USB_WAIT_INIT_US`, boot-only, sleeps in 1 ms slices) | Return false through PHY/core/role init; deinit returns early |
| Core reset self-clear, or v4.20a reset-done | 20 ms each (boot-only, sleeps) | Same; distinct counters for both revisions |
| `dwc2_common.h`: TX/RX FIFO flush | 20 ms in core/host/device init, 100 us at runtime (endpoint disable, bus reset) | Return false; callers stop before reusing FIFO state; close-all releases its lock |
| `hcd_dwc2.c`: forced host-mode acknowledgment | 150 ms, **boot only**, sleeps up to 1 ms between reads | Init returns false; long deadline accommodates hardware mode debounce |
| Channel disable / send-IN request queue, periodic and nonperiodic | 100 us | Return false through transfer submission or leave ISR immediately |
| HCD and DCD RX FIFO IRQ drain | 250 us per drain | Mask controller interrupts and return, even if RXFLVL is stuck or continuously replenished |
| `dcd_dwc2.c`: IN NAK, IN disable, global OUT NAK, OUT disable | 100 us each | Internal disable returns false; bool caller fails, void caller returns and health latch reports failure |
| `osal_none.h`: semaphore with no token | Immediate refusal | Return false and count it; never wait forever in a cooperative kernel |
| TinyUSB millisecond delay API | At most 1 ms spin | Requests longer than 1 ms are refused through the terminal fault latch; this port uses asynchronous timers |
| Host synchronous control APIs | Immediate refusal before retaining any pointers | Return false; the cooperative port requires completion callbacks, so neither synchronous polling loop is reachable |
| Host/device task event queues | Four events per call, zero queue timeout; host additionally checks a 1000 us cooperative budget (250 us reserve) | Return to the kernel; pending events remain queued |
| `drivers/usb.c`: OTG ID debounce | 100 ms at boot, sleeps in 1 ms slices | Samples ID then forces device; no OS runs during this boot-only window |

The remaining loops in `drivers/usb_host.c` traverse fixed device tables, at
most three report submissions, bounded descriptor buffers (512 bytes, with
strict forward progress), or fixed strings. Device CDC queues are bounded by
their allocated buffers; a write stops when no FIFO space remains. TinyUSB
FIFO/data-copy loops consume a finite byte/packet count; endpoint and channel
scans have fixed hardware/configuration limits. Host pending-control scans
consume the finite queue, and device-tree removal clears each marked device
and hub. None of these loops waits for hardware or an external event. Debug
printing loops are disabled (`CFG_TUSB_DEBUG=0`).

A hardware timeout is terminal for USB for the rest of the boot. The port
masks global/controller interrupts immediately; IRQ glue disables the CPU USB
IRQ, later enable calls are guarded, and foreground servicing stops the stack.
Host cleanup releases held keyboard keys. Subsequent submissions return false,
USB health becomes false, and the OS/audio/panel loop can continue. This avoids
re-entering the same wedged wait on every IRQ. Recovery requires reboot.

Each wait kind has a saturating counter. A terminal fault can replace the log tail if earlier messages filled it. IRQ code only updates counters and
the fault latch; foreground code formats lines such as
`DWC2 timeout RX IRQ drain count=1` into the existing USB.TXT log. Both roles
coalesce log updates for 1.5 seconds before saving, at most
12 times per boot, when storage is ready and no OS is running. While an OS is
running the evidence stays in RAM until a safe save opportunity. No disk I/O
or text formatting occurs in the IRQ.

Native tests compile the actual patched HCD, DCD and common code against a
fake register file. They withhold each awaited acknowledgment, check elapsed
fake microseconds (also across wrap), check the error and counter/log, then
exercise 1,000 later IRQ/submission calls without another wait. The heartbeat
integration test exercises the real register-wait helper and USB task wrapper
with the real OS supervisor, then runs 4,000 kernel ticks with health reports.
No physical-board measurements have been made; successful host tests/builds
do not establish hardware timing margins or USB interoperability.

Host interrupt NAK retries now use foreground microsecond deadlines with an
8 ms floor instead of enabling SOF IRQ countdowns. Successful report submissions
respect the same interval through the host scheduler; the DWC2 layer also guards
actual wire polls. The shared task budget admits work only before 750 us and
reserves 250 us within a 1000 us target. A callback already in progress and USB
IRQ preemption can exceed the target; `max_pass`/`over` explicitly retain that
evidence. One-second task/IRQ/endpoint counters and five-second status output
are described with the fake-HCD results in [USB host keyboard](usb-host-keyboard.md#tv-slowdown-polling-and-cpu-counters-2026-11-11).
