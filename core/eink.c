/* SPDX-License-Identifier: MIT. E-ink API rules (docs/eink.md): what the OS asked for, how often
 * it may ask, and when the kernel's own screen takes over. Pure logic, built for the host tests. */
#include "kernel.h"
#include "eink.h"
#include "render.h"
#include "font.h"
#define EAGAIN_ 11
#define EINVAL_ 22
static struct {
 bool has_title,has_mark,has_page,suppressed;
 char title[EINK_TITLE_MAX+1],build[EINK_BUILD_MAX+1]; uint8_t mark[EINK_MARK_BYTES],page[EINK_PAGE_BYTES];
 char seen[40]; /* the kernel error text the last redraw showed */
} s={.seen="none"}; /* the kernel starts with no error */
static uint64_t accepted[EINK_BURST]; static unsigned next_slot,filled; /* a ring of the last 30 accepted times */
static unsigned partials; static uint64_t last_full;
/* Gate every change: 1 s apart, 30 in any 60 s. A refusal changes nothing. */
static int gate(uint64_t now) {
 if(filled) {
  uint64_t last=accepted[(next_slot+EINK_BURST-1)%EINK_BURST];
  if(now-last<EINK_MIN_INTERVAL_US) return -EAGAIN_;
  if(filled==EINK_BURST&&now-accepted[next_slot]<EINK_BURST_WINDOW_US) return -EAGAIN_; /* next_slot holds the 30th most recent */
 }
 return 0;
}
static void take(uint64_t now) {accepted[next_slot]=now;next_slot=(next_slot+1)%EINK_BURST;if(filled<EINK_BURST) filled++;}
int eink_title(const char *name,uint32_t n,const uint8_t *mark,const char *build,uint32_t bn,uint64_t now) {
 if(!name||n<1||n>EINK_TITLE_MAX||bn>EINK_BUILD_MAX||(bn&&!build)) return -EINVAL_;
 char t[EINK_TITLE_MAX+1]; for(uint32_t i=0;i<n;i++) {
  char c=name[i]; if(c>='a'&&c<='z') c=(char)(c-32);
  if(c!=' '&&!font_glyph((uint8_t)c)) return -EINVAL_;
  t[i]=c;
 }
 t[n]=0;
 char b[EINK_BUILD_MAX+1]; for(uint32_t i=0;i<bn;i++) { /* free text: capitals, anything the font lacks shows as ? */
  char c=build[i]; if(c>='a'&&c<='z') c=(char)(c-32);
  b[i]=(c==' '||font_glyph((uint8_t)c))?c:'?';
 }
 b[bn]=0; int e=gate(now); if(e) return e;
 memcpy(s.title,t,n+1u); memcpy(s.build,b,bn+1u); s.has_title=true; s.has_mark=mark!=NULL; if(mark) memcpy(s.mark,mark,EINK_MARK_BYTES);
 take(now); return 0;
}
int eink_page(const uint8_t *bits,uint32_t bytes,uint64_t now) {
 if(!bits||bytes!=EINK_PAGE_BYTES) return -EINVAL_;
 int e=gate(now); if(e) return e;
 memcpy(s.page,bits,bytes); s.has_page=true; s.suppressed=false; take(now); return 0;
}
int eink_clear(uint64_t now) {
 int e=gate(now); if(e) return e;
 s.has_title=s.has_mark=s.has_page=s.suppressed=false; take(now); return 0;
}
void eink_reset(void) {s.has_title=s.has_mark=s.has_page=s.suppressed=false;}
bool eink_page_shown(void) {return s.has_page&&!s.suppressed;}
void eink_compose(uint8_t *plane,const char *build_id,const char *usb,const char *error,uint32_t seconds) {
 unsigned i=0; while(i<sizeof s.seen-1&&error[i]&&error[i]==s.seen[i]) i++;
 if(i<sizeof s.seen-1&&(error[i]||s.seen[i])) { /* a new kernel error: it wins at this very update */
  for(i=0;i<sizeof s.seen-1&&error[i];i++) s.seen[i]=error[i];
  s.seen[i]=0;
  if(s.has_page&&strcmp(error,"none")) s.suppressed=true;
 }
 if(s.has_page&&!s.suppressed) {render_page(plane,s.page,strcmp(error,"none")!=0);return;}
 render_status_ex(plane,s.has_title?s.title:NULL,s.has_title&&s.has_mark?s.mark:NULL,s.has_title?s.build:build_id,usb,error,seconds);
}
bool eink_full_due(uint64_t now) {return partials>=EINK_FULL_PARTIALS&&now-last_full>=EINK_FULL_MIN_US;}
void eink_note_refresh(bool full,uint64_t now) {if(full) {partials=0;last_full=now;} else partials++;}
