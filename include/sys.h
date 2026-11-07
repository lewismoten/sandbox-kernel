/* SPDX-License-Identifier: MIT; svc #0 dispatcher for the OS program (ABI
 * draft 1.0). Every call is checked with abi_check and the published records
 * before anything is done; user memory is reached only through uspace.h.
 * Paths: /sd/... (read-only assets and directories), /dev/log (write lines),
 * /dev/fb0, /dev/input/event0 (read-only, nonblocking). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define SYS_HANDLES 16u
#define SYS_LOG_LINE 120u
#define SYS_EXITED 1 /* sys_call result: the program called exit */
/* r[0..7] are the caller's registers. Returns SYS_EXITED, else the status for r0. */
int32_t sys_call(const uint32_t r[8]);
extern int64_t sys_exit_status;
extern uint32_t sys_calls; /* every call, counted */
extern uint32_t sys_healths; /* accepted health calls since the program started */
extern uint32_t sys_presents; /* framebuffer presents since the program started */
void sys_reset(void); /* close every handle and forget the surface */
uint32_t sys_open_handles(void);
