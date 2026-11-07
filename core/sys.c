/* SPDX-License-Identifier: MIT; original system-call dispatcher. Records are
 * copied in before use and copied out only on success; output fields must be
 * zero on entry; addresses are u64 with zero upper bits; transfers are at
 * most 1 MiB. Handles carry a generation, so a closed id is EBADF forever. */
#include "kernel.h"
#include "abi.h"
#include "assets.h"
#include "fb.h"
#include "sys.h"
#include "uspace.h"
#define EPERM 1
#define EBADF 9
#define ENOMEM 12
#define EACCES 13
#define EFAULT 14
#define EBUSY 16
#define ENODEV 19
#define EINVAL 22
#define EMFILE 24
#define ENOTTY 25
#define ENOENT 2
#define ENAMETOOLONG 36
#define ENOSYS 38
enum {K_FREE,K_LOG,K_FB,K_ASSET};
static struct {uint8_t kind; uint16_t gen; int asset;} fds[SYS_HANDLES];
static bool surface_mapped; static uint32_t surface_bytes;
int64_t sys_exit_status; uint32_t sys_calls,sys_presents;
void sys_reset(void) {
 for(unsigned i=0;i<SYS_HANDLES;i++) {if(fds[i].kind==K_ASSET) asset_close(fds[i].asset); fds[i].kind=K_FREE; fds[i].gen=(uint16_t)(fds[i].gen+1?fds[i].gen+1:1);}
 surface_mapped=false;surface_bytes=0;sys_presents=0;
}
uint32_t sys_open_handles(void) {uint32_t n=0;for(unsigned i=0;i<SYS_HANDLES;i++) n+=fds[i].kind!=K_FREE;return n;}
static int lookup(uint64_t fd) {
 if(fd>>24||!(fd>>8)) return -EBADF;
 unsigned i=(unsigned)(fd&0xffu);
 if(i>=SYS_HANDLES||fds[i].kind==K_FREE||fds[i].gen!=(uint16_t)(fd>>8)) return -EBADF;
 return (int)i;
}
static uint32_t fb_surface_bytes(void) {return fb0.width*4u*fb0.height;}
static int open_path(const char *p,uint32_t n,uint32_t flags,uint64_t *fd) {
 unsigned i=0; while(i<SYS_HANDLES&&fds[i].kind!=K_FREE) i++;
 if(i==SYS_HANDLES) return -EMFILE;
 uint32_t access=flags&3u;
 if(n==8&&!memcmp(p,"/dev/log",8)) {if(access==0) return -EACCES; fds[i].kind=K_LOG;}
 else if(n==8&&!memcmp(p,"/dev/fb0",8)) {
  if(!fb_pixels) return -ENODEV;
  for(unsigned j=0;j<SYS_HANDLES;j++) if(fds[j].kind==K_FB) return -EBUSY;
  fds[i].kind=K_FB;
 }
 else if(n>=3&&!memcmp(p,"/sd",3)&&(n==3||p[3]=='/')) {
  if(access==3) return -EINVAL;
  int h=n==3?asset_open("/",1,access):asset_open(p+3,n-3,access);
  if(h<0) return h;
  fds[i].kind=K_ASSET;fds[i].asset=h;
 }
 else if(n>=5&&!memcmp(p,"/dev/",5)) return -ENODEV;
 else return -ENOENT;
 if(!fds[i].gen) fds[i].gen=1;
 *fd=((uint64_t)fds[i].gen<<8)|i; return 0;
}
static void log_user(const uint8_t *s,uint32_t n) {
 char line[SYS_LOG_LINE+8]="OS: "; unsigned k=4;
 for(uint32_t i=0;i<n&&k<sizeof(line)-1;i++) {uint8_t c=s[i]; if(c=='\n'&&i+1==n) break; line[k++]=(char)(c>=32&&c<127?c:'?');}
 line[k]=0; log_line(line);
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
  m.width=fb0.width;m.height=fb0.height;m.pitch=fb0.width*4u;m.format=1;m.buffer_bytes=fb_surface_bytes();
  return us_copy_out(r->buffer,&m,sizeof m);
 }
 if(r->request==0x10001u) {
  struct k_fb_present q; int e=class_in(r->buffer,r->bytes,&q,sizeof q); if(e) return e;
  if(q.x>fb0.width||q.width>fb0.width-q.x||q.y>fb0.height||q.height>fb0.height-q.y) return -EINVAL;
  if(!surface_mapped) return -EINVAL; /* nothing drawn yet */
  uint8_t *src; if(us_span(U_FB_BASE,surface_bytes,U_R,&src)!=(int64_t)surface_bytes) return -EFAULT;
  for(uint32_t y=(uint32_t)q.y;y<q.y+q.height;y++) {
   const uint32_t *s=(const uint32_t *)(const void *)(src+(size_t)y*fb0.width*4u);
   volatile uint32_t *d=(volatile uint32_t *)(void *)(fb_pixels+(size_t)y*fb0.pitch);
   for(uint32_t x=(uint32_t)q.x;x<q.x+q.width;x++) {uint32_t c=s[x]&0xffffffu; d[x]=fb0.pixel_order?c:((c&0xff)<<16)|(c&0xff00)|((c>>16)&0xff);}
  }
  sys_presents++; return 0;
 }
 return -ENOTTY;
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
static const uint32_t sizes[17]={[1]=56,[2]=48,[3]=48,[4]=24,[5]=48,[6]=64,[16]=24};
int32_t sys_call(const uint32_t reg[8]) {
 sys_calls++;
 uint32_t call=reg[7],va=reg[0],bytes=reg[1];
 if(call>16||!sizes[call]) return -ENOSYS;
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
  case 2: case 3: if(((struct k_io *)rec)->transferred) return -EINVAL; break;
  case 6: if(((struct k_mmap *)rec)->address) return -EINVAL; break;
 }
 if((call==1||call==2||call==3||call==6)&&us_check(va,bytes,U_W)) return -EFAULT;
 switch(call) {
  case 1: {
   struct k_open *r=(struct k_open *)rec;
   if(r->mode) return -EINVAL;
   if(r->flags&~0x103ull) return -EINVAL;
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
   if(fds[i].kind==K_ASSET) asset_close(fds[i].asset);
   if(fds[i].kind==K_FB) {/* the surface stays mapped until the program ends */}
   fds[i].kind=K_FREE; fds[i].gen=(uint16_t)(fds[i].gen==0xffffu?1:fds[i].gen+1); return 0;
  }
  case 5: {
   struct k_ioctl *r=(struct k_ioctl *)rec; int i=lookup(r->fd); if(i<0) return i;
   if(fds[i].kind!=K_FB) return -ENOTTY;
   return ioctl_fb(r);
  }
  case 6: if((e=do_mmap((struct k_mmap *)rec))) return e; break;
  case 16: sys_exit_status=((struct k_exit *)rec)->status; return SYS_EXITED;
 }
 return us_copy_out(va,rec,bytes)?-EFAULT:0;
}
