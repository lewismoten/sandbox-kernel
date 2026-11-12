/* SPDX-License-Identifier: MIT. The kernel's idle screen and program launcher (include/launcher.h).
 * Draws into a pixel buffer through fb_info like core/fb_art.c, so the host renders the same
 * screen to PNG. Text is the kernel's 5x7 font, upper case only. */
#include "kernel.h"
#include "launcher.h"
#include "font.h"
static const struct launcher_ops *ops;
static bool on;
static unsigned count,sel;
static char names[LAUNCHER_MAX][LAUNCHER_NAME];
static char title[40],msg[40],foot[40];
static void put(char *d,unsigned max,const char *s) {unsigned i=0;for(;s[i]&&i+1<max;i++)d[i]=s[i];d[i]=0;}
static void scan(void) {count=ops?ops->list(names):0;if(count>LAUNCHER_MAX)count=LAUNCHER_MAX;if(sel>=count)sel=0;}
void launcher_open(const struct launcher_ops *o,const char *stopped) {
 ops=o;on=true;sel=0;msg[0]=0;scan();
 if(!stopped||!stopped[0]) put(title,sizeof title,"PROGRAMS");
 else if(!strcmp(stopped,"/OS/GMOS.ELF")) put(title,sizeof title,"GAME-MAKER OS STOPPED");
 else {const char *b=stopped;for(const char *p=stopped;*p;p++)if(*p=='/')b=p+1;char t[40]="";put(t,14,b);unsigned n=0;while(t[n])n++;memcpy(t+n,t[0]?" STOPPED":"PROGRAMS",9);put(title,sizeof title,t);}
}
void launcher_close(void) {on=false;}
bool launcher_active(void) {return on;}
bool launcher_key(unsigned code) {
 if(!on) return false;
 if(code==LAUNCHER_KEY_UP) {if(count>1) {sel=sel?sel-1:count-1;msg[0]=0;return true;}return false;}
 if(code==LAUNCHER_KEY_DOWN) {if(count>1) {sel=sel+1<count?sel+1:0;msg[0]=0;return true;}return false;}
 if(code!=LAUNCHER_KEY_ENTER&&code!=LAUNCHER_KEY_KPENTER&&code!=LAUNCHER_KEY_OK&&code!=LAUNCHER_KEY_SELECT) return false;
 if(!count) {scan();msg[0]=0;return true;} /* nothing listed: Enter looks again */
 char path[20]="/OS/";put(path+4,16,names[sel]);
 if(ops&&ops->run&&ops->run(path)==0) {on=false;return false;}
 put(msg,sizeof msg,"COULD NOT START THAT ONE");
 return true;
}
unsigned launcher_count(void) {return count;}
unsigned launcher_selected(void) {return sel;}
const char *launcher_name(unsigned i) {return i<count?names[i]:"";}
const char *launcher_title(void) {return title;}
const char *launcher_message(void) {return msg;}
void launcher_footer(const char *t) {put(foot,sizeof foot,t?t:"");}
static uint32_t *at(const struct fb_info *f,uint8_t *p,uint32_t x,uint32_t y) {return (uint32_t *)(void *)(p+(size_t)y*f->pitch+(size_t)x*4u);}
static void rect(const struct fb_info *f,uint8_t *p,uint32_t x0,uint32_t y0,uint32_t w,uint32_t h,uint32_t rgb) {
 uint32_t c=fb_native(f,rgb);
 for(uint32_t y=y0;y<y0+h;y++) {volatile uint32_t *r=at(f,p,x0,y);for(uint32_t x=0;x<w;x++) r[x]=c;}
}
/* A string at scale s (glyph 5x7 pixels times s, advance 6 columns); returns the x after it. */
static uint32_t text(const struct fb_info *f,uint8_t *p,uint32_t x,uint32_t y,unsigned s,uint32_t rgb,const char *str) {
 for(;*str;str++,x+=6*s) {
  if(*str==' ') continue;
  const uint8_t *g=font_glyph((uint8_t)*str);
  for(unsigned r=0;r<7;r++) for(unsigned c=0;c<5;c++) {
   bool dot=!g?(r==0||r==6||c==0||c==4):(g[r]>>(4-c))&1;
   if(dot) rect(f,p,x+c*s,y+r*s,s,s,rgb);
  }
 }
 return x;
}
bool launcher_draw(const struct fb_info *f,uint8_t *p) {
 if(f->depth!=32||f->width!=960||f->height!=540) return false;
 rect(f,p,0,0,960,540,0x101420);
 rect(f,p,24,24,912,2,0x7888a0);rect(f,p,24,514,912,2,0x7888a0);
 text(f,p,48,48,4,0xffffff,title);
 rect(f,p,48,92,864,2,0x7888a0);
 if(!count) {
  text(f,p,48,150,3,0xffffff,"NO PROGRAMS FOUND IN /OS");
  text(f,p,48,200,2,0xa8b8d0,"COPY AN .ELF OVER USB, THEN PRESS ENTER");
  text(f,p,48,226,2,0xa8b8d0,"TO LOOK AGAIN.");
 } else {
  unsigned first=sel>=LAUNCHER_ROWS?sel-LAUNCHER_ROWS+1:0;
  for(unsigned i=0;i<LAUNCHER_ROWS&&first+i<count;i++) {
   unsigned k=first+i;uint32_t y=116+i*40;bool hot=k==sel;
   if(hot) rect(f,p,48,y-6,864,36,0x305090);
   if(hot) for(unsigned r=0;r<4;r++) rect(f,p,64+r*4,y+r*3,4,21-r*6,0xffffff); /* a right-pointing arrow */
   uint32_t x=text(f,p,100,y,3,hot?0xffffff:0xc8d4e8,"/OS/");
   text(f,p,x,y,3,hot?0xffffff:0xc8d4e8,names[k]);
  }
  if(count>LAUNCHER_ROWS) {
   if(first>0) for(unsigned r=0;r<5;r++) rect(f,p,884-r*4,108+r*4,r*8+4,4,0xa8b8d0); /* more above */
   if(first+LAUNCHER_ROWS<count) for(unsigned r=0;r<5;r++) rect(f,p,884-(4-r)*4,400+r*4,(4-r)*8+4,4,0xa8b8d0); /* more below */
  }
 }
 if(msg[0]) text(f,p,48,424,2,0xff9090,msg);
 rect(f,p,48,452,864,2,0x7888a0);
 text(f,p,48,466,2,0xa8b8d0,count?"UP/DOWN CHOOSE   ENTER START":"ENTER LOOK AGAIN");
 text(f,p,48,488,2,0xffffff,"CONNECT USB TO UPDATE");
 uint32_t n=0;while(foot[n])n++;
 text(f,p,912-n*12,488,2,0xa8b8d0,foot);
 return true;
}
