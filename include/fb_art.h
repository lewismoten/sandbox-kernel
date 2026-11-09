/* SPDX-License-Identifier: MIT. HDMI boot overlay and recovery console (see core/fb_art.c). */
#pragma once
#include "fb.h"
/* Draw the title panel over a finished test pattern: 960x540 and 640x480, 32 bpp; other sizes are left alone. */
void fb_overlay(const struct fb_info *f,uint8_t *pixels);
/* The recovery console: header and a 60x12 log box showing the last log lines; 960x540, 32 bpp only
   (returns false for any other size, drawing nothing). */
bool fb_console(const struct fb_info *f,uint8_t *pixels);
void fb_console_log(const struct fb_info *f,uint8_t *pixels); /* redraw only the log box */
/* The kernel's log lines, kept for the console: long lines wrap at 60 columns. */
void console_log(const char *line);
uint32_t console_gen(void); /* changes whenever a line is added */
