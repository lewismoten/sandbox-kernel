/* SPDX-License-Identifier: MIT; what the JS runtime needs from the machine it
 * runs on: the clock, files, the log, the 640x480 surface and input. The board
 * (plat_sbx.c, sandbox-kernel system calls) and the host tests
 * (tests/host/plat_host.c) each give one. */
#pragma once
/* Drop buffered events at an app/launcher boundary, without changing devices. */
void plat_reset_input(void);
#include <stdint.h>
#define PLAT_W 640u /* the mode a program starts in: 640x480, 32 bpp */
#define PLAT_H 480u
#define PLAT_SURFACE (640u * 480u * 4u) /* every mode fits in it (960x540 at 16 bpp does) */
uint64_t plat_clock_ns(void);           /* monotonic */
void plat_sleep_until(uint64_t ns);     /* returns early on input where it can */
void plat_heartbeat(void);              /* the kernel's health call; nothing on the host */
void plat_log(const char *s, uint32_t n);
/* Reads up to cap bytes of a file ("/sd/..." on the board); bytes read or -1. */
int32_t plat_read(const char *path, uint8_t *buf, uint32_t cap);
/* Lists a folder: calls fn for each entry (dir = 1 for folders); count or -1. */
int32_t plat_list(const char *path, void (*fn)(void *user, const char *name, uint32_t n, int dir, uint32_t size), void *user);
void *plat_fb(void);                    /* the mode's pixels, rows of width x depth/8 bytes */
/* Another mode (the kernel's ABI 1.2; the firmware scales it to HDMI): depth
   32 (XRGB8888), 16 (RGB565) or 8 (palette indices). 0, or -1. The time the
   switch took goes to *us. */
int plat_set_mode(uint32_t w, uint32_t h, uint32_t depth, uint32_t *us);
int plat_palette(const uint8_t *pal); /* 256 entries, 6-bit VGA values */
void plat_present(uint32_t x, uint32_t y, uint32_t w, uint32_t h);
/* The next input event (Linux type/code/value): 1, or 0 when none is waiting. */
int plat_event(uint16_t *type, uint16_t *code, int32_t *value);
