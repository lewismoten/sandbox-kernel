/* SPDX-License-Identifier: MIT. The OS's e-ink API (ABI 1.9; docs/eink.md): a title, a page, the
 * panel-care limits and the kernel's right to take the panel back. No hardware in eink.c. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define EINK_W 250u
#define EINK_H 122u
#define EINK_PAGE_BYTES 3904u /* 122 rows of 32 bytes, bit 7 = leftmost, 1 = black */
#define EINK_TITLE_MAX 16u
#define EINK_BUILD_MAX 16u /* the build line: 16 characters at 12 px from x=54 end at the panel edge */
#define EINK_MARK_BYTES 200u /* 40x40, 5-byte rows */
#define EINK_MIN_INTERVAL_US 1000000ull   /* VERIFY: between accepted updates */
#define EINK_BURST 30u                    /* VERIFY: accepted updates in any 60 s */
#define EINK_BURST_WINDOW_US 60000000ull
#define EINK_FULL_PARTIALS 50u            /* VERIFY: partials before a ghost-clearing full refresh */
#define EINK_FULL_MIN_US 180000000ull     /* Waveshare 2.13" V4: full refreshes at least 180 s apart */
#define EINK_FULL_MAX_US 82800000000ull   /* 23 h: the panel's at-least-daily full refresh, with an hour of margin */
/* All return 0 or a negative errno (-22 EINVAL, -11 EAGAIN); the boot refresh counts as a full one (eink_note_refresh(true) at its command trigger); now_us is the monotonic microseconds. */
int eink_title(const char *name,uint32_t name_bytes,const uint8_t *mark,const char *build,uint32_t build_bytes,uint64_t now_us); /* mark NULL: none; build_bytes 0: a blank build line */
int eink_page(const uint8_t *bits,uint32_t bytes,uint64_t now_us);
int eink_clear(uint64_t now_us);
void eink_reset(void); /* the program ended (exit, fault, stop): the kernel has the panel back */
/* The screen to draw now: the page, the title screen or the kernel's own. */
void eink_compose(uint8_t *plane,const char *build_id,const char *usb,const char *error,uint32_t seconds);
void eink_set_available(bool on); /* the panel driver: running (true) or absent/disabled (false); the OS API reports ENODEV while false */
bool eink_available(void);
bool eink_maintenance_due(uint64_t now_us);   /* 23 h since the last full refresh: redraw even if nothing changed */
bool eink_full_due(uint64_t now_us);          /* the next redraw should be a full refresh */
void eink_note_refresh(bool full,uint64_t now_us);
uint32_t eink_generation(void);               /* changes whenever the OS asked for a new picture or the kernel error text changed */
#define EINK_REDRAW_MIN_US 10000000ull   /* the kernel status screen redraws at most this often, unless eink_generation changed */
#define EINK_SLEEP_SHOW_US 30000000ull   /* power saving must hold this long before the panel says SLEEP */
#define EINK_SLEEP_CLEAR_US 10000000ull  /* and be gone this long before SLEEP clears */
bool eink_page_shown(void);                   /* test hook: is an OS page what compose draws */
