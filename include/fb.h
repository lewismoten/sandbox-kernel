/* SPDX-License-Identifier: MIT; HDMI framebuffer allocation through the
 * firmware property mailbox. fb_request/fb_parse/fb_pattern are pure and host
 * tested; fb_start (drivers/fb_hw.c) talks to the board. */
#pragma once
#include <stdint.h>
/* The mode every program starts in (ABI 1.0 programs know no other). */
#define FB_WIDTH 640u
#define FB_HEIGHT 480u
#define FB_DEPTH 32u
/* The kernel's own screen (the test pattern at boot and after a fault):
   16:9, so a 1080p monitor shows it doubled and edge to edge. */
#define FB_HOME_WIDTH 960u
#define FB_HOME_HEIGHT 540u
#define FB_HOME_DEPTH 32u
#define FB_MESSAGE_WORDS 36u
/* The program's surface is this size in every mode (640x480 at 32 bpp); a
   mode set with SBX_IOC_FB_SET_MODE must fit in it (960x540 at 16 bpp does). */
#define FB_SURFACE_MAX (640u*480u*4u)
#define FB_PALETTE_WORDS (8u+256u)
struct fb_info {
 uint32_t width,height,depth,pitch,bytes,pixel_order;
 uint32_t bus,phys; /* GPU bus address as returned, ARM physical (bus & 0x3fffffff) */
};
/* Pixel order as the firmware reports it describes bytes in memory, low address
 * first: 0 ("BGR") stores B,G,R,X, which is the little-endian word 0x00RRGGBB
 * that programs and the test pattern write; 1 ("RGB") stores R,G,B,X, so the
 * same word shows red and blue swapped (seen on HDMI, 2026-11-07). */
#define FB_ORDER_BGR 0u
static inline uint32_t fb_native(const struct fb_info *f,uint32_t c) {
 c&=0xffffffu; return f->pixel_order==FB_ORDER_BGR?c:((c&0xff)<<16)|(c&0xff00)|((c>>16)&0xff);
}
void fb_request(uint32_t m[FB_MESSAGE_WORDS],uint32_t width,uint32_t height,uint32_t depth);
/* NULL when the reply is a usable buffer, else a short failure code. */
const char *fb_parse(const uint32_t m[FB_MESSAGE_WORDS],uint32_t depth,struct fb_info *out);
/* Eight color bars over a 16-step gray ramp, with a white border. */
void fb_pattern(const struct fb_info *fb,uint8_t *pixels);
uint32_t fb_pattern_pixel(const struct fb_info *fb,uint32_t x,uint32_t y);
/* The firmware's set-palette message (tag 0x4800b) for count entries from
   first, each 0x00RRGGBB, sent as R,G,B,A bytes; returns the message words. */
uint32_t fb_palette_request(uint32_t m[FB_PALETTE_WORDS],uint32_t first,uint32_t count,const uint32_t *rgb);
/* Another mode from the firmware (depth 8, 16 or 32; it scales the buffer to
   the HDMI mode), mapped and cleared: 0 or a negative errno. fb_program_mode
   puts 640x480x32 up for a program's start (when it fails, the screen is
   given up: fb_pixels NULL, so no program writes past its surface);
   fb_home puts the kernel's own mode and test pattern back (after a fault). */
int fb_set_mode(uint32_t width,uint32_t height,uint32_t depth);
int fb_set_palette(uint32_t first,uint32_t count,const uint32_t *rgb);
void fb_program_mode(void);
void fb_home(void);
void fb_boot_screen(const struct fb_info *fb,uint8_t *pixels); /* bars and ramp, then the title panel */
void fb_recovery(void); /* the log console; fb_recovery_task redraws its log box as lines arrive */
void fb_recovery_task(void);
extern uint32_t fb_mode_us; /* how long the last mode set took */
extern const char *fb_fail;
extern struct fb_info fb0;
extern uint8_t *fb_pixels; /* kernel pointer to the mapped buffer, or NULL */
bool fb_start(void); /* allocate, map, draw, log; never touches USB or the panel */
