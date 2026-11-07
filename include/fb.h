/* SPDX-License-Identifier: MIT; HDMI framebuffer allocation through the
 * firmware property mailbox. fb_request/fb_parse/fb_pattern are pure and host
 * tested; fb_start (drivers/fb_hw.c) talks to the board. */
#pragma once
#include <stdint.h>
#define FB_WIDTH 640u
#define FB_HEIGHT 480u
#define FB_DEPTH 32u
#define FB_MESSAGE_WORDS 36u
struct fb_info {
 uint32_t width,height,depth,pitch,bytes,pixel_order;
 uint32_t bus,phys; /* GPU bus address as returned, ARM physical (bus & 0x3fffffff) */
};
void fb_request(uint32_t m[FB_MESSAGE_WORDS],uint32_t width,uint32_t height,uint32_t depth);
/* NULL when the reply is a usable buffer, else a short failure code. */
const char *fb_parse(const uint32_t m[FB_MESSAGE_WORDS],uint32_t depth,struct fb_info *out);
/* Eight color bars over a 16-step gray ramp, with a white border. */
void fb_pattern(const struct fb_info *fb,uint8_t *pixels);
uint32_t fb_pattern_pixel(const struct fb_info *fb,uint32_t x,uint32_t y);
extern const char *fb_fail;
extern struct fb_info fb0;
extern uint8_t *fb_pixels; /* kernel pointer to the mapped buffer, or NULL */
bool fb_start(void); /* allocate, map, draw, log; never touches USB or the panel */
