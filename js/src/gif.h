/* SPDX-License-Identifier: MIT; GIF pictures for the pages between scenes
 * (title, level intro and exit, won, lost, backdrops), as PLAYGAME's GIFC.C
 * draws them: the first image only, palette reduced to 6-bit VGA values. */
#pragma once
#include <stdint.h>
/* Draws the first image over a 320 x 200 screen (px, 64000 indices) with the
   image's pixel (sx, sy) at the top left, as the Sandbox Edition's drawImage
   does: screen pixels the image does not cover are left as they were. Any
   image size. pal gets the palette (768, 6-bit). 0, or -1 when it is not a GIF. */
int gp_gif_draw(const uint8_t *d, uint32_t n, int32_t sx, int32_t sy, uint8_t *pal, uint8_t *px);
int gp_gif(const uint8_t *d, uint32_t n, uint8_t *pal, uint8_t *px); /* at (0, 0) */
/* The palette alone: 0, or -1 when it is not a GIF. */
int gp_gif_palette(const uint8_t *d, uint32_t n, uint8_t *pal);
