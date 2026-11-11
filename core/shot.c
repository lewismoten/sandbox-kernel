/* SPDX-License-Identifier: MIT; original screen capture, foreground only. */
#include "kernel.h"
#include "fb.h"
#include "shot.h"
uint32_t fb_shot_palette[256];
bool fb_shot_palette_known[256];
static uint8_t panel_copy[4000];
static uint32_t panel_id;
const char *shot_read(bool eink,uint32_t scale,uint32_t id,uint32_t offset,struct shot_chunk *s) {
 if(offset%SHOT_CHUNK)return "shot range";
 if(eink) {
  if(!id) {
   if(offset)return "shot range";
   if(!panel_snapshot(panel_copy))return "shot unavailable";
   if(!++panel_id)++panel_id;
  } else if(id!=panel_id)return "shot stale";
  s->id=panel_id;s->width=122;s->height=250;s->stride=16;s->depth=1;
  s->order=panel_snapshot_order();s->total=4000;
 } else {
  if(scale<1||scale>16)return "shot range";
  if(!fb_pixels||!fb_mode_generation)return "shot unavailable";
  if((!id&&offset)||(id&&id!=fb_mode_generation))return "shot stale";
  if(!fb0.width||!fb0.height||fb0.width>4096||fb0.height>4096||
     (fb0.depth!=8&&fb0.depth!=16&&fb0.depth!=32)||fb0.pixel_order>1)return "shot unavailable";
  uint32_t row=fb0.width*(fb0.depth/8);
  if(fb0.pitch<row||(uint64_t)fb0.pitch*fb0.height>fb0.bytes)return "shot unavailable";
  s->id=fb_mode_generation;s->width=(fb0.width+scale-1)/scale;s->height=(fb0.height+scale-1)/scale;
  s->depth=fb0.depth;s->order=fb0.pixel_order;s->stride=s->width*(s->depth/8);
  uint64_t total=(uint64_t)s->stride*s->height+(s->depth==8?1024:0);
  if(total>SHOT_MAX)return "shot range";
  s->total=(uint32_t)total;
 }
 if(offset>=s->total)return "shot range";
 s->offset=offset;s->count=s->total-offset;if(s->count>SHOT_CHUNK)s->count=SHOT_CHUNK;
 if(eink)memcpy(s->data,panel_copy+offset,s->count);
 else for(uint32_t i=0;i<s->count;i++) {
  uint32_t p=offset+i;
  if(s->depth==8&&p<1024) {s->data[i]=(uint8_t)(fb_shot_palette[p/4]>>((p%4)*8));continue;}
  if(s->depth==8)p-=1024;
  uint32_t bpp=s->depth/8,y=p/s->stride,x=(p%s->stride)/bpp;
  uint8_t v=((volatile uint8_t *)fb_pixels)[(size_t)y*scale*fb0.pitch+x*scale*bpp+p%bpp];
  if(s->depth==8&&!fb_shot_palette_known[v])return "shot palette";
  s->data[i]=v;
 }
 return NULL;
}
