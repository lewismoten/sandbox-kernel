/* SPDX-License-Identifier: MIT; Game-Maker OS bitmap font (tools/font.txt):
 * printable ASCII 32..126, 5x9 cells (capitals 7 rows tall, 2 descender
 * rows), bit 4 of each row byte is the leftmost column. */
#pragma once
#include <stdint.h>
#define GM_FONT_W 5u
#define GM_FONT_H 9u
extern const uint8_t gm_font[95][9];
