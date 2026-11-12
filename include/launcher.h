/* SPDX-License-Identifier: MIT. The kernel's idle screen and program launcher: drawn after the OS
 * program exits (or when none autostarts), listing the .ELF programs in /OS. Pure logic and
 * drawing here (core/launcher.c, host-tested with a fake directory and run); the loop that
 * feeds it keys and the card is core/launcher_task.c. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "fb.h"
#define LAUNCHER_MAX 16u       /* programs kept */
#define LAUNCHER_ROWS 8u       /* rows shown at once; the list scrolls */
#define LAUNCHER_NAME 13u      /* "GMOS.ELF" and the like: 8.3 plus NUL */
#define LAUNCHER_KEY_UP 103u   /* Linux key codes: USB keyboards and the CEC remote arrive as these */
#define LAUNCHER_KEY_DOWN 108u
#define LAUNCHER_KEY_ENTER 28u
#define LAUNCHER_KEY_KPENTER 96u
#define LAUNCHER_KEY_OK 352u
#define LAUNCHER_KEY_SELECT 353u
struct launcher_ops {
 /* Fill names (NUL-terminated, at most LAUNCHER_NAME bytes) with the runnable programs; the count. */
 unsigned (*list)(char names[LAUNCHER_MAX][LAUNCHER_NAME]);
 /* Start /OS/<name> through the supervisor; 0 or a negative errno. */
 int (*run)(const char *path);
};
/* Show the screen: stopped is the path of the program that just ended, or NULL. Lists once. */
void launcher_open(const struct launcher_ops *ops,const char *stopped);
void launcher_close(void);
bool launcher_active(void);
/* A key went down (or repeated). Up/Down choose; Enter starts the choice (or looks again when the list is empty).
   Returns true when the screen needs a redraw. A successful start closes the screen. */
bool launcher_key(unsigned code);
unsigned launcher_count(void);
unsigned launcher_selected(void);
const char *launcher_name(unsigned i);
const char *launcher_title(void);
const char *launcher_message(void); /* "" or the last start failure */
void launcher_footer(const char *kernel_text); /* the kernel line at the bottom, e.g. "KERNEL GEN 4 AB12CD" */
/* 960x540, 32 bpp only (false otherwise, nothing drawn). */
bool launcher_draw(const struct fb_info *f,uint8_t *pixels);
/* Main-loop side (core/launcher_task.c; kernel image only). */
void launcher_request(const char *stopped); /* show the screen once the supervisor is idle; stopped may be NULL */
const char *launcher_eink_state(void);      /* e-ink "USB:" line while shown, else NULL */
void launcher_task(bool storage_ready,uint32_t generation,const char *build_id);
