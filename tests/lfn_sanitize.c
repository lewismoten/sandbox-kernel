/* SPDX-License-Identifier: MIT; standalone ASan/UBSan over real core+fake SD. */
#include "kernel.h"
#include "assets.h"
#include "abi.h"
#include "sys.h"
#include "uspace.h"
#include "vfat.h"
#include <assert.h>
#include <stdio.h>
extern void open_disk(const char *path),close_disk(void);
extern uint32_t write_count(void);
extern uint32_t *kernel_table(void);
#define REC U_IMAGE_BASE
#define PATH (REC+4096)
#define OUT (REC+8192)
static int invoke(unsigned call,void *rec,unsigned bytes) {
 assert(!us_copy_out(REC,rec,bytes));
 uint32_t r[8]={REC,bytes,1,0,0,0,0,call};int result=sys_call(r);
 assert(!us_copy_in(rec,REC,bytes));return result;
}
static void scan(const char *path,unsigned depth) {
 assert(depth<=16);
 struct k_open o={{sizeof o,0,0,0},PATH,strlen(path),0,0,0};
 assert(!us_copy_out(PATH,path,(uint32_t)o.path_bytes));assert(!invoke(1,&o,sizeof o));
 for(;;) {
  /* Repeated too-small calls must not lose the pending long entry. */
  struct k_io tiny={{sizeof tiny,0,0,0},o.fd,OUT,1,0};
  int r=invoke(20,&tiny,sizeof tiny);assert(r==-22||(!r&&!tiny.transferred));
  uint8_t guard[288];memset(guard,0xa5,sizeof guard);assert(!us_copy_out(OUT,guard,sizeof guard));
  struct k_io io={{sizeof io,0,0,0},o.fd,OUT,280,0};
  assert(!invoke(20,&io,sizeof io));assert(io.transferred<=280);
  assert(!us_copy_in(guard,OUT,sizeof guard));for(unsigned j=280;j<sizeof guard;j++)assert(guard[j]==0xa5);
  if(!io.transferred)break;
  for(unsigned off=0;off<io.transferred;) {
  struct k_dirent_meta d;memcpy(&d,guard+off,sizeof d);
  assert(d.length<=io.transferred-off&&d.length%8==0&&d.length>=24u+d.name_bytes&&d.name_bytes);
  assert(!d.reserved);
  for(unsigned j=24+d.name_bytes;j<d.length;j++)assert(!guard[off+j]);
  const uint8_t *name=guard+off+24;off+=d.length;
  char child[256];unsigned n=(unsigned)strlen(path);
  if(n+1+d.name_bytes>255)continue;
  memcpy(child,path,n);if(path[n-1]!='/')child[n++]='/';memcpy(child+n,name,d.name_bytes);n+=d.name_bytes;child[n]=0;
  /* Raw OEM short aliases remain opaque; their code page is not inferred. */
  bool ascii=true;for(unsigned j=0;j<d.name_bytes;j++)if(name[j]>=128)ascii=false;

  if(d.type==2)scan(child,depth+1);
  else {
   struct k_open f={{sizeof f,0,0,0},PATH,n,0,0,0};assert(!us_copy_out(PATH,child,n));
   int e=invoke(1,&f,sizeof f);if(!ascii&&e==-22)continue;assert(!e);
   struct k_io read={{sizeof read,0,0,0},f.fd,OUT,512,0};assert(!invoke(2,&read,sizeof read));
   struct k_close cl={{sizeof cl,0,0,0},f.fd};assert(!invoke(4,&cl,sizeof cl));
  }
  }
 }
 struct k_close cl={{sizeof cl,0,0,0},o.fd};assert(!invoke(4,&cl,sizeof cl));
}
static uint32_t rng=0x1f4a9u;
static uint32_t random32(void) {rng^=rng<<13;rng^=rng>>17;rng^=rng<<5;return rng;}
int main(int argc,char **argv) {
 assert(argc==2);open_disk(argv[1]);assert(!asset_mount());
 assert(!us_reset(kernel_table()));assert(!us_map(REC,16384,U_R|U_W));
 scan("/sd/",0);assert(!sys_open_handles());assert(!write_count());close_disk();
 /* Mutated raw slots always enter through the actual bounded state machine.
  * Valid decoded results must be strict UTF-8 and reflexively equal. */
 for(unsigned round=0;round<100000;round++) {
  struct vfat_name n;vfat_reset(&n);uint8_t e[32];char name[256];
  unsigned slots=1+random32()%24;
  for(unsigned j=0;j<slots;j++) {
   for(unsigned k=0;k<32;k++)e[k]=(uint8_t)random32();
   if(round%2==0) {e[0]=(uint8_t)((slots-j)|(j?0:64));e[11]=15;e[12]=0;e[13]=0;e[26]=e[27]=0;}
   vfat_slot(&n,e);
  }
  memset(e,' ',11);e[11]=32;
  if(vfat_decode(&n,e,name)) {assert(vfat_component(name,(unsigned)strlen(name)));assert(vfat_equal(name,(unsigned)strlen(name),name));}
 }
 puts("VFAT ASan/UBSan: real FAT, syscall20/pending/280-byte guards, reads, 100000 seeded slot campaigns OK");
 return 0;
}
