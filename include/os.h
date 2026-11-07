/* SPDX-License-Identifier: MIT; the OS program supervisor: load an ELF from
 * the card into User mode, run it a timer slice at a time from the kernel's
 * main loop, dispatch its calls, and apply the restart policy when it exits
 * or faults. The kernel loop (USB, panel, watchdog, trial) keeps running
 * between slices, so a looping or crashing program cannot stop it. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
struct uframe {uint32_t r[13],sp,lr,pc,cpsr;}; /* layout used by arch/armv6/start.S */
enum {OS_SVC=1,OS_IRQ=2,OS_UNDEF=3,OS_PABT=4,OS_DABT=5};
/* Heartbeat (docs/design.md): the first health call is the readiness report
   and must come within 10 s of the start; after it, health calls at least
   every 2 s. Missing either is a fault. Other calls do not count. */
#define OS_READY_US 10000000u
#define OS_HEARTBEAT_US 2000000u
#define OS_FAULT_WINDOW_US 60000000ull
extern struct uframe os_frame;
extern const char *os_fail;
extern char os_path[32];
int os_start(const char *path); /* load and prepare; 0 or a negative errno */
bool os_running(void);
void os_handle(unsigned reason,uint32_t fsr,uint32_t far); /* one return from User mode */
void os_task(void); /* board: run one slice, then handle why it ended */
/* Restart policy: after a fault (or nonzero exit), true means restart once;
   a second fault within 60 s keeps the kernel recovery console. */
struct os_policy {uint32_t faults; uint64_t first_us;};
bool os_policy_restart(struct os_policy *p,uint64_t now_us);
extern struct os_policy os_policy_state;
extern uint32_t os_starts,os_stops;
