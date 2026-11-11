/* SPDX-License-Identifier: MIT; bounded read-only screen capture. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define SHOT_CHUNK 256u
#define SHOT_MAX (4u*1024u*1024u)
/* Last successfully triggered panel plane; no optical readback. */
bool panel_snapshot(uint8_t out[4000]);
uint32_t panel_snapshot_order(void);
/* Palette shadow: unknown indices are refused rather than guessed. */
extern uint32_t fb_shot_palette[256];
extern bool fb_shot_palette_known[256];
struct shot_chunk {
 uint32_t id,width,height,stride,depth,order,total,offset,count;
 uint8_t data[SHOT_CHUNK];
};
const char *shot_read(bool eink,uint32_t scale,uint32_t id,uint32_t offset,struct shot_chunk *out);
