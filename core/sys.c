/* SPDX-License-Identifier: MIT; original system-call dispatcher. Records are
 * copied in before use and copied out only on success; output fields must be
 * zero on entry; addresses are u64 with zero upper bits; transfers are at
 * most 1 MiB. Handles carry a generation, so a closed id is EBADF forever. */
#include "kernel.h"
#include "power.h"
#include "serial.h"
#include "clock.h"
#include "eink.h"
#include "abi.h"
#include "assets.h"
#include "fatput.h"
#include "fb.h"
#include "input.h"
#include "audio_null.h"
#include "sys.h"
#include "uspace.h"
#include "os.h"
#define EPERM 1
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define EAGAIN 11
#define ENODEV 19
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ESPIPE 29
#define ENOENT 2
#define ENAMETOOLONG 36
#define ENOSYS 38
#define ENOTDIR 20
enum {K_FREE,K_LOG,K_FB,K_ASSET,K_INPUT,K_CACHE,K_EINK,K_AUDIO,K_SERIAL};
static uint8_t eink_stage[EINK_PAGE_BYTES]; /* a page copied in before the rules see it */
/* One bounded write transaction; never a general filesystem writer. */
static uint8_t cache_bytes[FATPUT_MAX];
static char cache_name[13],cache_game[9];static uint32_t cache_used;static bool cache_dirty; /* cache_game "" = /OS/CACHE */
static bool cache_writing(void);
static int cache_path(const char *p,uint32_t n,char out[13]) {
 if(n<14||n>25||memcmp(p,"/sd/OS/CACHE/",13))return -30;
 for(unsigned i=0;i<n;i++)if(!p[i])return -EINVAL;
 unsigned k=n-13;memset(out,0,13);memcpy(out,p+13,k);
 uint8_t nm[11];return fatput_name(out,nm);
}
/* /sd/OS/SAVES/<GAME>/<NAME>: GAME 1-8 of A-Z 0-9 _ - (no dot), NAME a plain 8.3 name. */
static int save_path(const char *p,uint32_t n,char game[9],char out[13]) {
 if(n<15||n>34||memcmp(p,"/sd/OS/SAVES/",13))return -30;
 for(unsigned i=0;i<n;i++)if(!p[i])return -EINVAL;
 uint32_t g=13;while(g<n&&p[g]!='/')g++;
 if(g==13||g-13>8||g+1>=n||n-g-1>12)return -EINVAL;
 memset(game,0,9);memcpy(game,p+13,g-13);memset(out,0,13);memcpy(out,p+g+1,n-g-1);
 uint8_t nm[11];int e=fatput_name(game,nm);if(e)return e;
 for(const char *c=game;*c;c++)if(*c=='.')return -EINVAL;
 return fatput_name(out,nm);
}
/* Either writable folder: the cache (game "") or one game's saves. */
static int write_path(const char *p,uint32_t n,char game[9],char out[13]) {
 game[0]=0;
 if(n>=13&&!memcmp(p,"/sd/OS/SAVES/",13))return save_path(p,n,game,out);
 return cache_path(p,n,out);
}
/* inst: the input instance an input handle was opened on; pend: a directory
   entry that did not fit the last getdents buffer. */
/* Eight slot bits leave 56 generation bits. Exhausted slots stay retired, including across reset. */
#define HANDLE_GEN_MAX (UINT64_MAX >> 8)
static struct {uint8_t kind; bool has_pend; uint64_t gen; int asset; uint32_t inst; struct asset_long_dirent pend; uint32_t meta[3];} fds[SYS_HANDLES];
static bool surface_mapped; static uint32_t surface_bytes;
int64_t sys_exit_status; uint32_t sys_calls,sys_presents,sys_healths; uint64_t sys_health_seq,sys_wake_us;
extern uint32_t boot_generation;
static void release(unsigned i) {
 if(fds[i].kind==K_ASSET) asset_close(fds[i].asset);
 if(fds[i].kind==K_INPUT) input_close(fds[i].inst);
 if(fds[i].kind==K_SERIAL) serial_close();
 if(fds[i].kind==K_AUDIO) audio_dev_close();
 fds[i].kind=K_FREE;fds[i].has_pend=false;
 if(fds[i].gen<=HANDLE_GEN_MAX) fds[i].gen++; /* MAX+1 is the permanent retirement marker */
}
static bool cache_writing(void) {for(unsigned i=0;i<SYS_HANDLES;i++)if(fds[i].kind==K_CACHE)return true;return false;}
void sys_reset(void) {
 power_reset(clock_us());
 for(unsigned i=0;i<SYS_HANDLES;i++) release(i);
 eink_reset(); /* the kernel has the panel back, whatever ended the program */
 surface_mapped=false;surface_bytes=0;sys_presents=0;sys_healths=0;sys_health_seq=0;sys_wake_us=0;
}
uint32_t sys_open_handles(void) {uint32_t n=0;for(unsigned i=0;i<SYS_HANDLES;i++) n+=fds[i].kind!=K_FREE;return n;}
static int lookup(uint64_t fd) {
 if(!(fd>>8)) return -EBADF;
 unsigned i=(unsigned)(fd&0xffu);
 if(i>=SYS_HANDLES||fds[i].kind==K_FREE||fds[i].gen!=(fd>>8)) return -EBADF;
 return (int)i;
}
static uint32_t fb_surface_bytes(void) {return FB_SURFACE_MAX;} /* the same in every mode */
static uint32_t fb_bpp(void) {return fb0.depth/8u;}
static int open_path(const char *p,uint32_t n,uint32_t flags,uint64_t *fd) {
 if((flags&0x600u)&&flags!=0x601u)return -EINVAL;
 if((flags&0x600u)&&(n<13||(memcmp(p,"/sd/OS/CACHE/",13)&&memcmp(p,"/sd/OS/SAVES/",13))))return -30;
 unsigned i=0; while(i<SYS_HANDLES&&(fds[i].kind!=K_FREE||fds[i].gen>HANDLE_GEN_MAX)) i++;
 if(i==SYS_HANDLES) return -EMFILE;
 uint32_t access=flags&3u;
 if(n==8&&!memcmp(p,"/dev/log",8)) {if(access==0) return -EACCES; fds[i].kind=K_LOG;}
 else if(n==8&&!memcmp(p,"/dev/fb0",8)) {
  if(!fb_pixels) return -ENODEV;
  for(unsigned j=0;j<SYS_HANDLES;j++) if(fds[j].kind==K_FB) return -EBUSY;
  fds[i].kind=K_FB;
 }
 else if(n==12&&!memcmp(p,"/dev/serial0",12)) {
  if(access==3) return -EINVAL;
  if(!(flags&0x100u)) return -EINVAL; /* nonblocking only */
  int e=serial_open(); if(e) return e;
  fds[i].kind=K_SERIAL;
 }
 else if(n==9&&!memcmp(p,"/dev/eink",9)) { /* ABI 1.9: write-only, one handle (docs/eink.md) */
  if(!eink_available()) return -ENODEV;
  if(access!=1) return -EACCES;
  for(unsigned j=0;j<SYS_HANDLES;j++) if(fds[j].kind==K_EINK) return -EBUSY;
  fds[i].kind=K_EINK;
 }
 else if(n==10&&!memcmp(p,"/dev/audio",10)) { /* ABI 1.8: write-only, one writer */
  if(access!=1) return -EACCES;
  int e=audio_dev_open(); if(e) return e;
  fds[i].kind=K_AUDIO;
 }
 else if(n==17&&!memcmp(p,"/dev/input/event0",17)) {
  if(access) return -EACCES;
  if(!(flags&0x100u)) return -EINVAL; /* blocking reads need poll, which comes later */
  uint32_t inst=input_instance(); int e=input_open(inst); if(e) return e;
  fds[i].kind=K_INPUT;fds[i].inst=inst;
 }
 else if(n>=3&&!memcmp(p,"/sd",3)&&(n==3||p[3]=='/')) {
  if(flags&0x600u) {
   if(flags!=0x601u)return -EINVAL; /* WRONLY|CREAT|TRUNC, no append/update */
   char name[13],game[9];int e=write_path(p,n,game,name);if(e)return e;
   if(cache_writing()||asset_cache_readers())return -EBUSY;
   memcpy(cache_name,name,13);memcpy(cache_game,game,9);cache_used=0;cache_dirty=true;fds[i].kind=K_CACHE;
   goto opened;
  }
  if(access==3) return -EINVAL;
  int h=n==3?asset_open("/",1,access):asset_open(p+3,n-3,access);
  if(h<0) return h;
  fds[i].kind=K_ASSET;fds[i].asset=h;
 }
 else if(n>=5&&!memcmp(p,"/dev/",5)) return -ENODEV;
 else return -ENOENT;
opened:
 if(!fds[i].gen) fds[i].gen=1;
 *fd=((uint64_t)fds[i].gen<<8)|i; return 0;
}
static char recent[SYS_RECENT][SYS_LOG_LINE+8];static uint32_t recent_next;
const char *sys_recent_log(unsigned back) {return back<SYS_RECENT&&back<recent_next?recent[(recent_next-1-back)%SYS_RECENT]:"";}
static void log_user(const uint8_t *s,uint32_t n) {
 char line[SYS_LOG_LINE+8]="OS: "; unsigned k=4;
 for(uint32_t i=0;i<n&&k<sizeof(line)-1;i++) {uint8_t c=s[i]; if(c=='\n'&&i+1==n) break; line[k++]=(char)(c>=32&&c<127?c:'?');}
 line[k]=0; log_line(line);
 memcpy(recent[recent_next%SYS_RECENT],line+4,k-3);recent_next++;
}
static int io(struct k_io *r,bool write) {
 int i=lookup(r->fd); if(i<0) return i;
 if(r->count>(1u<<20)) return -EINVAL;
 if(r->buffer>>32) return -EFAULT;
 uint32_t va=(uint32_t)r->buffer,count=(uint32_t)r->count;
 if(us_check(va,count,write?U_R:U_W)) return -EFAULT;
 if(fds[i].kind==K_LOG) {
  if(!write) return -EBADF;
  uint8_t line[SYS_LOG_LINE]; uint32_t take=count<SYS_LOG_LINE?count:SYS_LOG_LINE;
  us_copy_in(line,va,take); log_user(line,take); r->transferred=count; return 0;
 }
 if(fds[i].kind==K_SERIAL) {
  uint8_t b[256]; uint32_t done=0;
  if(write) {
   while(done<count) {uint32_t t=count-done<sizeof b?count-done:(uint32_t)sizeof b; if(us_copy_in(b,va+done,t)) return -EFAULT; serial_write(b,t); done+=t;}
   r->transferred=count; return 0;
  }
  int got=serial_read(b,count<sizeof b?count:(uint32_t)sizeof b); if(got<0) return got;
  if(us_copy_out(va,b,(uint32_t)got)) return -EFAULT;
  r->transferred=(uint32_t)got; return 0;
 }
 if(fds[i].kind==K_EINK) {
  if(!write) return -EBADF;
  if(!eink_available()) return -ENODEV;
  if(count!=EINK_PAGE_BYTES) return -EINVAL; /* a whole page or nothing */
  if(us_copy_in(eink_stage,va,count)) return -EFAULT;
  int e=eink_page(eink_stage,count,clock_us()); if(e) return e;
  r->transferred=count; return 0;
 }
 if(fds[i].kind==K_INPUT) {
  if(write) return -EBADF;
  if(count<sizeof(struct input_event_rec)) return -EINVAL; /* whole records only */
  struct input_event_rec ev[16]; uint32_t max=count/16u; if(max>16) max=16;
  int got=input_read(fds[i].inst,ev,max); if(got<0) return got;
  if(us_copy_out(va,ev,(uint32_t)got*16u)) return -EFAULT;
  r->transferred=(uint32_t)got*16u; return 0;
 }
 if(fds[i].kind==K_AUDIO) {
  /* Whole stereo S16 frames only. Never blocks: takes what fits and says how
     many bytes; nothing fitting is EAGAIN (the app retries after a short wait). */
  static uint8_t pcm[2048];
  if(!write) return -EBADF;
  if(count&3u) return -EINVAL;
  uint32_t done=0;
  while(done<count) {
   uint32_t room=audio_ring_free(&audio0.ring)*AUDIO_FRAME_BYTES;
   uint32_t take=count-done; if(take>sizeof pcm) take=sizeof pcm; if(take>room) take=room;
   if(!take) break;
   if(us_copy_in(pcm,va+done,take)) return -EFAULT;
   done+=audio_dev_write(pcm,take)*AUDIO_FRAME_BYTES;
  }
  if(done<count) audio_ring_refuse(&audio0.ring,(count-done)/AUDIO_FRAME_BYTES);
  if(!done&&count) return -EAGAIN;
  r->transferred=done; return 0;
 }
 if(fds[i].kind==K_CACHE) {
  if(!write)return -EBADF;
  if(count>FATPUT_MAX-cache_used)return -28;
  if(us_copy_in(cache_bytes+cache_used,va,count))return -EFAULT;
  cache_used+=count;cache_dirty=true;r->transferred=count;return 0;
 }
 if(fds[i].kind!=K_ASSET) return -EINVAL;
 if(write) return -EBADF;
 uint32_t done=0;
 while(done<count) {
  uint8_t *p; int64_t span=us_span(va+done,count-done,U_W,&p); if(span<=0) return -EFAULT;
  int got=asset_read(fds[i].asset,p,(uint32_t)span);
  if(got<0) {if(done) break; return got;}
  done+=(uint32_t)got; if((int64_t)got<span) break;
 }
 r->transferred=done; return 0;
}
/* Copy-in and check an ioctl class record (at most 64 bytes). */
static int class_in(uint64_t va,uint64_t bytes,void *rec,uint32_t size) {
 if(va>>32||bytes>4096) return bytes>4096?-EINVAL:-EFAULT;
 if(va&7) return -EFAULT;
 if(bytes!=size) return -EINVAL; /* this kernel knows one size per class record */
 if(us_copy_in(rec,va,size)) return -EFAULT;
 return abi_check(rec,(uint32_t)bytes,K_ABI_MAJOR,size,size);
}
static int ioctl_fb(const struct k_ioctl *r) {
 if(r->request==0x10000u) {
  struct k_fb_mode m; int e=class_in(r->buffer,r->bytes,&m,sizeof m); if(e) return e;
  if(m.width|m.height|m.pitch|m.format|m.buffer_bytes) return -EINVAL;
  if(us_check(r->buffer,sizeof m,U_W)) return -EFAULT;
  m.width=fb0.width;m.height=fb0.height;m.pitch=fb0.width*fb_bpp();m.format=fb0.depth==32?1:fb0.depth==16?2:3;
  m.buffer_bytes=fb0.depth==32?fb_surface_bytes():m.pitch*fb0.height;
  return us_copy_out(r->buffer,&m,sizeof m);
 }
 if(r->request==0x10001u) {
  struct k_fb_present q; int e=class_in(r->buffer,r->bytes,&q,sizeof q); if(e) return e;
  if(q.x>fb0.width||q.width>fb0.width-q.x||q.y>fb0.height||q.height>fb0.height-q.y) return -EINVAL;
  if(!surface_mapped) return -EINVAL; /* nothing drawn yet */
  uint8_t *src; if(us_span(U_FB_BASE,surface_bytes,U_R,&src)!=(int64_t)surface_bytes) return -EFAULT;
  uint32_t bpp=fb_bpp(),x0=(uint32_t)q.x*bpp,x1=(uint32_t)(q.x+q.width)*bpp; /* bytes of a row */
  for(uint32_t y=(uint32_t)q.y;y<q.y+q.height;y++) {
   const uint8_t *s=src+(size_t)y*fb0.width*bpp; uint8_t *d=fb_pixels+(size_t)y*fb0.pitch;
   if(bpp==4) {
    const uint32_t *s4=(const uint32_t *)(const void *)s; volatile uint32_t *d4=(volatile uint32_t *)(void *)d;
    for(uint32_t x=(uint32_t)q.x;x<q.x+q.width;x++) d4[x]=fb_native(&fb0,s4[x]);
    continue;
   }
   uint32_t b=x0; /* 16 and 8 bpp: the bytes as they are, a word at a time where aligned */
   for(;b<x1&&(b&3);b++) ((volatile uint8_t *)d)[b]=s[b];
   for(;b+4<=x1;b+=4) *(volatile uint32_t *)(void *)(d+b)=*(const uint32_t *)(const void *)(s+b);
   for(;b<x1;b++) ((volatile uint8_t *)d)[b]=s[b];
  }
  sys_presents++; return 0;
 }
 if(r->request==0x10002u) { /* ABI 1.2: another mode (the firmware scales it to the HDMI mode) */
  struct k_fb_set m; int e=class_in(r->buffer,r->bytes,&m,sizeof m); if(e) return e;
  uint32_t d=m.format==1?32u:m.format==2?16u:m.format==3?8u:0u;
  if(!d||m.width<16||m.height<16||m.width>1920||m.height>1200||m.width*m.height*(d/8u)>FB_SURFACE_MAX||m.width%4) return -EINVAL;
  return fb_set_mode((uint32_t)m.width,(uint32_t)m.height,d);
 }
 if(r->request==0x10003u) { /* ABI 1.2: palette entries for 8-bit indices */
  struct k_fb_palette m; int e=class_in(r->buffer,r->bytes,&m,sizeof m); if(e) return e;
  if(!m.count||m.first>255||m.count>256-m.first) return -EINVAL;
  return fb_set_palette((uint32_t)m.first,(uint32_t)m.count,m.rgb);
 }
 return -ENOTTY;
}
 static int ioctl_serial(const struct k_ioctl *r) {
 if(r->request!=0x60000u) return -ENOTTY;
 struct k_serial_info q; int e=class_in(r->buffer,r->bytes,&q,sizeof q); if(e) return e;
 if(q.transport|q.tx_bytes|q.tx_dropped|q.rx_bytes|q.rx_dropped|q.tx_pending|q.rx_pending) return -EINVAL;
 if(us_check(r->buffer,sizeof q,U_W)) return -EFAULT;
 struct serial_counters c; serial_counters(&c);
 q.transport=c.transport;q.tx_bytes=c.tx_bytes;q.tx_dropped=c.tx_dropped;q.rx_bytes=c.rx_bytes;q.rx_dropped=c.rx_dropped;q.tx_pending=c.tx_pending;q.rx_pending=c.rx_pending;
 return us_copy_out(r->buffer,&q,sizeof q);
}
static int ioctl_eink(const struct k_ioctl *r) {
 uint64_t now=clock_us();
 if(!eink_available()) return -ENODEV;
 if(r->request==0x40000u) {
  struct k_eink_info q; int e=class_in(r->buffer,r->bytes,&q,sizeof q); if(e) return e;
  if(q.width|q.height|q.page_bytes|q.title_max|q.mark_bytes|q.min_interval_ms|q.max_per_minute|q.flags) return -EINVAL;
  if(us_check(r->buffer,sizeof q,U_W)) return -EFAULT;
  q.width=EINK_W;q.height=EINK_H;q.page_bytes=EINK_PAGE_BYTES;q.title_max=EINK_TITLE_MAX;q.mark_bytes=EINK_MARK_BYTES;
  q.min_interval_ms=EINK_MIN_INTERVAL_US/1000u;q.max_per_minute=EINK_BURST;
  return us_copy_out(r->buffer,&q,sizeof q);
 }
 if(r->request==0x40001u) {
  struct k_eink_title t; int e=class_in(r->buffer,r->bytes,&t,sizeof t); if(e) return e;
  if((t.flags&~1ull)||t.name_bytes<1||t.name_bytes>sizeof t.name||t.build_bytes>sizeof t.build) return -EINVAL;
  for(uint64_t i=t.build_bytes;i<sizeof t.build;i++) if(t.build[i]) return -EINVAL;
  for(uint64_t i=t.name_bytes;i<sizeof t.name;i++) if(t.name[i]) return -EINVAL;
  if(!(t.flags&1u)) for(unsigned i=0;i<sizeof t.mark;i++) if(t.mark[i]) return -EINVAL;
  return eink_title(t.name,(uint32_t)t.name_bytes,(t.flags&1u)?t.mark:NULL,t.build,(uint32_t)t.build_bytes,now);
 }
 if(r->request==0x40002u) {
  struct k_header h; int e=class_in(r->buffer,r->bytes,&h,sizeof h); if(e) return e;
  return eink_clear(now);
 }
 return -ENOTTY;
}
static int ioctl_audio(const struct k_ioctl *r) {
 if(r->request!=0x20000u) return -ENOTTY;
 struct k_audio_info q; int e=class_in(r->buffer,r->bytes,&q,sizeof q); if(e) return e;
 if(q.flags|q.rate|q.capacity_frames|q.queued_frames|q.played_frames|q.underruns|q.overruns|q.overrun_frames) return -EINVAL;
 if(us_check(r->buffer,sizeof q,U_W)) return -EFAULT;
 const struct audio_ring *a=&audio0.ring;
 q.rate=AUDIO_RING_RATE;q.capacity_frames=AUDIO_RING_FRAMES;q.queued_frames=audio_ring_queued(a);
 q.played_frames=a->played;q.underruns=a->underruns;q.overruns=a->overruns;q.overrun_frames=a->overrun_frames;
 return us_copy_out(r->buffer,&q,sizeof q);
}
static int ioctl_input(unsigned i,const struct k_ioctl *r) {
 if(r->request==0x30000u) {
  struct k_input_info q; int e=class_in(r->buffer,r->bytes,&q,sizeof q); if(e) return e;
  if(q.flags|q.instance|q.queue_records|q.keys|q.axes|(uint64_t)q.axis_min|(uint64_t)q.axis_max) return -EINVAL;
  if(us_check(r->buffer,sizeof q,U_W)) return -EFAULT;
  if(!input_present()||fds[i].inst!=input_instance()) return -ENODEV;
  q.flags=INPUT_SYNTHETIC;q.instance=fds[i].inst;q.queue_records=INPUT_QUEUE;q.keys=INPUT_KEYS;q.axes=INPUT_AXES;q.axis_min=INPUT_AXIS_MIN;q.axis_max=INPUT_AXIS_MAX;
  return us_copy_out(r->buffer,&q,sizeof q);
 }
 if(r->request==0x30001u) {
  static struct k_input_state q; int e=class_in(r->buffer,r->bytes,&q,sizeof q); if(e) return e;
  for(unsigned k=0;k<12;k++) if(q.keys[k]) return -EINVAL;
  for(unsigned k=0;k<32;k++) if(q.axes[k]) return -EINVAL;
  if(us_check(r->buffer,sizeof q,U_W)) return -EFAULT;
  if((e=input_state(fds[i].inst,q.keys,q.axes))) return e;
  return us_copy_out(r->buffer,&q,sizeof q);
 }
 return -ENOTTY;
}
/* getdents: whole entries {u16 length, u8 type, u8 name bytes, u32 size, name,
   zero padding to 8}; "." and ".." are left out. An entry that does not fit
   waits for the next call; a buffer too small for it is EINVAL. */
static int getdents(struct k_io *r,bool metadata,bool long_names) {
 /* Stack ownership invalidates at entry/exit, on ALL success/error paths.
  * Only call20 shares bytes between entries; calls11/18 retain old behavior. */
 struct asset_dir_sector sector;sector.lba=~0u;
 int i=lookup(r->fd); if(i<0) return i;
 if(fds[i].kind!=K_ASSET) return -ENOTDIR;
 if(r->count>(1u<<20)) return -EINVAL;
 if(r->buffer>>32) return -EFAULT;
 uint32_t va=(uint32_t)r->buffer,count=(uint32_t)r->count,done=0;
 if(us_check(va,count,U_W)) return -EFAULT;
 for(;;) {
  struct asset_long_dirent d;uint32_t meta[3];
  if(fds[i].has_pend) {d=fds[i].pend;memcpy(meta,fds[i].meta,sizeof meta);fds[i].has_pend=false;}
  else {int e=long_names?asset_readdir_long_batch(fds[i].asset,&d,meta,&sector):asset_readdir_long(fds[i].asset,&d,meta); if(e<0) {if(done) break; return e;} if(!e) break;}
  const char *name=long_names?d.name:d.short_entry.name;
  uint32_t nb=(uint32_t)strlen(name);
  uint32_t head=metadata?24u:8u,len=(head+nb+7u)&~7u;
  if(len>count-done) {fds[i].pend=d;memcpy(fds[i].meta,meta,sizeof meta);fds[i].has_pend=true; if(!done) return -EINVAL; break;}
  uint8_t e[280]; memset(e,0,sizeof e);
  struct k_dirent h={(uint16_t)len,(uint8_t)(d.short_entry.is_dir?2:1),(uint8_t)nb,d.short_entry.size}; memcpy(e,&h,8);
  if(metadata)memcpy(e+8,meta,sizeof meta);
  memcpy(e+head,name,nb);
  if(us_copy_out(va+done,e,len)) return -EFAULT;
  done+=len;
 }
 r->transferred=done; return 0;
}
static int health(const struct k_health *r) {
 if(r->generation!=boot_generation) return -EINVAL;
 if(r->sequence<=sys_health_seq) return -EINVAL; /* strictly increasing */
 sys_health_seq=r->sequence;sys_healths++;return 0;
}
static int do_mmap(struct k_mmap *r) {
 int i=lookup(r->fd); if(i<0) return r->fd==~0ull?-ENOSYS:i; /* anonymous mappings come later */
 if(fds[i].kind!=K_FB) return -ENODEV;
 if(r->offset||r->flags!=1u||r->protection!=3u) return r->protection&4u?-EACCES:-EINVAL;
 uint32_t need=(fb_surface_bytes()+4095u)&~4095u;
 if(!r->length||r->length>need) return -EINVAL;
 if(!surface_mapped) {int e=us_map(U_FB_BASE,need,U_R|U_W); if(e) return e==-12?-ENOMEM:e; surface_mapped=true;surface_bytes=fb_surface_bytes();}
 r->address=U_FB_BASE; return 0;
}
/* clock_gettime (13): selector 0, monotonic time since the kernel started, in ns
   (microsecond steps; the same clock as input event times). */
static int clock_get(struct k_clock *r) {
 if(r->clock) return -EINVAL;
 r->ns=clock_us()*1000u; return 0;
}
/* sleep_until (14): the program is not run again until the monotonic clock
   reaches the deadline (ns, absolute); a deadline already passed returns at
   once. The kernel loop keeps serving USB and the panel meanwhile. Waiting
   is not progress: the heartbeat deadline still runs. */
static int sleep_until(const struct k_sleep *r) {
 uint64_t n=r->deadline>~0ull-999u?~0ull:r->deadline+999u,us=0,rem=0; /* ceil(ns / 1000) without a 64-bit divide helper */
 for(int i=63;i>=0;i--) {rem=(rem<<1)|((n>>i)&1u);us<<=1;if(rem>=1000u) {rem-=1000u;us|=1u;}}
 sys_wake_us=us>clock_us()?us:0; return 0;
}
static const uint32_t sizes[24]={[1]=56,[2]=48,[3]=48,[4]=24,[5]=48,[6]=64,[9]=48,[11]=48,[12]=24,[13]=32,[14]=24,[16]=24,[17]=32,[18]=48,[19]=48,[20]=48,[21]=48,[22]=40,[23]=32};
int32_t sys_call(const uint32_t reg[8]) {
 sys_calls++;
 uint32_t call=reg[7],va=reg[0],bytes=reg[1];
 if(call>23||!sizes[call]) return -ENOSYS;
 if(reg[3]|reg[4]|reg[5]|reg[6]) return -EINVAL;
 if(reg[2]!=K_ABI_MAJOR) return -93;
 if(bytes<16||bytes>4096||bytes%8) return -EINVAL;
 if(va&7) return -EFAULT;
 static uint64_t rec[512]; /* 8-byte aligned kernel copy */
 if(us_copy_in(rec,va,bytes)) return -EFAULT;
 uint32_t known=sizes[call];
 int e=abi_check(rec,bytes,K_ABI_MAJOR,known,known); if(e) return e;
 /* outputs must be zero on entry; the record must be writable if it has any */
 switch(call) {
  case 1: if(((struct k_open *)rec)->fd) return -EINVAL; break;
  case 2: case 3: case 11: case 18: case 20: if(((struct k_io *)rec)->transferred) return -EINVAL; break;
  case 6: if(((struct k_mmap *)rec)->address) return -EINVAL; break;
  case 9: if(((struct k_seek *)rec)->position) return -EINVAL; break;
  case 13: if(((struct k_clock *)rec)->ns) return -EINVAL; break;
  case 21: if(((struct k_app_arena *)rec)->address|((struct k_app_arena *)rec)->actual_bytes) return -EINVAL; break;
 }
 if((call==1||call==2||call==3||call==6||call==9||call==11||call==13||call==18||call==20||call==21||call==22)&&us_check(va,bytes,U_W)) return -EFAULT;
 switch(call) {
  case 1: {
   struct k_open *r=(struct k_open *)rec;
   if(r->mode) return -EINVAL;
   if(r->flags&~0x703ull) return -EINVAL;
   if(r->path>>32) return -EFAULT;
   if(!r->path_bytes) return -EINVAL;
   if(r->path_bytes>255) return -ENAMETOOLONG;
   char path[256]; if(us_copy_in(path,r->path,(uint32_t)r->path_bytes)) return -EFAULT;
   for(uint32_t i=0;i<r->path_bytes;i++) if(!path[i]) return -EINVAL;
   uint64_t fd; e=open_path(path,(uint32_t)r->path_bytes,(uint32_t)r->flags,&fd); if(e) return e;
   r->fd=fd; break;
  }
  case 2: case 3: if((e=io((struct k_io *)rec,call==3))) return e; break;
  case 4: {
   int i=lookup(((struct k_close *)rec)->fd); if(i<0) return i;
   release((unsigned)i); return 0; /* the fb0 surface stays mapped until the program ends */
  }
  case 5: {
   struct k_ioctl *r=(struct k_ioctl *)rec; int i=lookup(r->fd); if(i<0) return i;
   if(fds[i].kind==K_INPUT) return ioctl_input((unsigned)i,r);
   if(fds[i].kind==K_SERIAL) return ioctl_serial(r);
   if(fds[i].kind==K_EINK) return ioctl_eink(r);
   if(fds[i].kind==K_AUDIO) return ioctl_audio(r);
   if(fds[i].kind!=K_FB) return -ENOTTY;
   return ioctl_fb(r);
  }
  case 6: if((e=do_mmap((struct k_mmap *)rec))) return e; break;
  case 9: {
   struct k_seek *r=(struct k_seek *)rec; int i=lookup(r->fd); if(i<0) return i;
   if(fds[i].kind!=K_ASSET) return -ESPIPE;
   if(r->whence||r->offset<0) return -EINVAL;
   if((e=asset_seek(fds[i].asset,(uint64_t)r->offset))) return e;
   r->position=(uint64_t)r->offset;break;
  }
  case 11: case 18: case 20: if((e=getdents((struct k_io *)rec,call!=11,call==20))) return e; break;
  case 12: {
   int i=lookup(((struct k_close *)rec)->fd);if(i<0)return i;
   if(fds[i].kind!=K_CACHE)return -EINVAL;
   if(!cache_dirty)return 0;
   e=cache_game[0]?fatsave_put(cache_game,cache_name,cache_bytes,cache_used):fatcache_put(cache_name,cache_bytes,cache_used);
   if(!e)cache_dirty=false;
   return e; /* sd_write waits for card programming and readback at each sector */
  }
  case 19: {
   const struct k_rename *r=(const struct k_rename *)rec;
   if(r->from>>32||r->to>>32)return -EFAULT;
   if(r->from_bytes>255||r->to_bytes>255)return -ENAMETOOLONG;
   char a[256],b[256],na[13],nb[13],ga[9],gb[9];
   if(us_copy_in(a,r->from,(uint32_t)r->from_bytes)||us_copy_in(b,r->to,(uint32_t)r->to_bytes))return -EFAULT;
   if((e=write_path(a,(uint32_t)r->from_bytes,ga,na))||(e=write_path(b,(uint32_t)r->to_bytes,gb,nb)))return e;
   if(strcmp(ga,gb))return -EINVAL; /* never across folders */
   if(cache_writing()||asset_cache_readers())return -EBUSY;
   return ga[0]?fatsave_rename(ga,na,nb,cache_bytes,sizeof cache_bytes):fatcache_rename(na,nb,cache_bytes,sizeof cache_bytes);
  }
  case 13: if((e=clock_get((struct k_clock *)rec))) return e; break;
  case 14: return sleep_until((const struct k_sleep *)rec);
  case 17: return health((const struct k_health *)rec);
  case 16: sys_exit_status=((struct k_exit *)rec)->status; return SYS_EXITED;
  case 22: {
   struct k_power_hold *r=(struct k_power_hold *)rec;
   if(r->operation==1) {
    if(r->token) return -EINVAL;
    if((e=power_acquire(r->flags,&r->token))) return e;
   } else if(r->operation==0) {
    if(r->flags) return -EINVAL;
    return power_release(r->token);
   } else if(r->operation==2) { /* ABI 1.12: which resources are asleep now (POWER_NO_* bits), for the app to stop its work */
    if(r->token||r->flags) return -EINVAL;
    r->flags=power_target(clock_us());
   } else return -EINVAL;
   break;
  }
  case 23: {
   const struct k_power_timeouts *r=(const struct k_power_timeouts *)rec;
   return power_timeouts(r->slowdown_seconds,r->blank_seconds,clock_us());
  }
  case 21: return os_app_arena(va,(struct k_app_arena *)rec,bytes);
 }
 e=us_copy_out(va,rec,bytes);
 if(e&&call==22) power_release(((struct k_power_hold *)rec)->token);
 return e?-EFAULT:0;
}
