/* SPDX-License-Identifier: MIT. The OS's e-ink API (ABI 1.9; docs/eink.md): a title, a page, the
 * panel-care limits and the kernel's right to take the panel back. No hardware in eink.c. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define EINK_W 250u
#define EINK_H 122u
#define EINK_PAGE_BYTES 3904u /* 122 rows of 32 bytes, bit 7 = leftmost, 1 = black */
#define EINK_TITLE_MAX 16u
#define EINK_MARK_BYTES 200u /* 40x40, 5-byte rows */
#define EINK_MIN_INTERVAL_US 1000000ull   /* VERIFY: between accepted updates */
#define EINK_BURST 30u                    /* VERIFY: accepted updates in any 60 s */
#define EINK_BURST_WINDOW_US 60000000ull
#define EINK_FULL_PARTIALS 50u            /* VERIFY: partials before a ghost-clearing full refresh */
#define EINK_FULL_MIN_US 180000000ull     /* Waveshare 2.13" V4: full refreshes at least 180 s apart */
/* All return 0 or a negative errno (-22 EINVAL, -11 EAGAIN); now_us is the monotonic microseconds. */
int eink_title(const char *name,uint32_t name_bytes,const uint8_t *mark,uint64_t now_us); /* mark NULL: none */
int eink_page(const uint8_t *bits,uint32_t bytes,uint64_t now_us);
int eink_clear(uint64_t now_us);
void eink_reset(void); /* the program ended (exit, fault, stop): the kernel has the panel back */
/* The screen to draw now: the page, the title screen or the kernel's own. */
void eink_compose(uint8_t *plane,const char *build_id,const char *usb,const char *error,uint32_t seconds);
bool eink_full_due(uint64_t now_us);          /* the next redraw should be a full refresh */
void eink_note_refresh(bool full,uint64_t now_us);
bool eink_page_shown(void);                   /* test hook: is an OS page what compose draws */
