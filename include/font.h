/* SPDX-License-Identifier: MIT. The 5x7 font: 7 rows of 5 bits, bit 4 = leftmost column. */
#pragma once
#include <stdint.h>
/* NULL for a character the font lacks (callers draw a box). Lowercase maps to uppercase. */
const uint8_t *font_glyph(unsigned ch);
