/* SPDX-License-Identifier: MIT; original restricted FAT32 writer for /OS.
 * Order makes a power cut safe for every existing file: the new clusters
 * are free until the FAT links them, the directory entry switches to them
 * last, and only then is the old chain released. A cut can leak clusters
 * (a disk check reclaims them) but cannot cross-link or truncate a file.
 * Both FAT copies are updated one sector at a time; FSInfo's free-cluster
 * count moves by the clusters taken and released (an unknown count stays
 * unknown). */
#include "kernel.h"
#include "assets.h"
#include "fatput.h"
const char *fatput_fail="none";
static struct asset_volume v;
static uint8_t s[512],chk[512];
static uint32_t chain[FATPUT_MAX/512+1],old[FATPUT_MAX/512+1],taken;
static uint16_t u16(const uint8_t *p) {return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t u32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static void put16(uint8_t *p,uint32_t x) {p[0]=(uint8_t)x;p[1]=(uint8_t)(x>>8);}
static void put32(uint8_t *p,uint32_t x) {for(int i=0;i<4;i++) p[i]=(uint8_t)(x>>(8*i));}
static int fail(const char *w,int e) {fatput_fail=w;return -e;}
static bool cluster_ok(uint32_t c) {return c>=2&&c<v.clusters+2;}
static uint32_t lba_of(uint32_t c,uint32_t i) {return v.data+(c-2)*v.spc+i;}
static int rd(uint32_t lba,uint8_t *b) {return lba>=v.base&&lba<v.end&&sd_read(lba,b)?0:fail("PUT READ",5);}
/* Writes reach only the FAT copies, the data area, or the FSInfo sector. */
static int wr(uint32_t lba,const uint8_t *b,uint32_t fsinfo) {
 if(!((lba>=v.fat&&lba<v.data)||(lba>=v.data&&lba<v.end)||(fsinfo&&lba==fsinfo))) return fail("PUT RANGE",5);
 if(!sd_write(lba,b)) return fail("PUT WRITE",5);
 if(!sd_read(lba,chk)||memcmp(chk,b,512)) return fail("PUT READBACK",5);
 return 0;
}
/* One cached sector from each FAT copy: a scan for free clusters reads each
   FAT sector once, not once per cluster (the watchdog allows 8 s). */
static uint8_t fc[2][512]; static uint32_t fc_sec=~0u;
static int fat_get(uint32_t c,uint32_t *out) {
 uint32_t sec=c/128; int r;
 if(sec>=v.fat_size) return fail("PUT FAT RANGE",5);
 if(sec!=fc_sec) {fc_sec=~0u; if((r=rd(v.fat+sec,fc[0]))||(r=rd(v.fat+v.fat_size+sec,fc[1]))) return r; fc_sec=sec;}
 uint32_t a=u32(fc[0]+(c%128)*4)&0x0fffffffu;
 if((u32(fc[1]+(c%128)*4)&0x0fffffffu)!=a) return fail("PUT FAT MISMATCH",5);
 *out=a; return 0;
}
static int fat_set(uint32_t c,uint32_t val) {
 fc_sec=~0u;
 for(uint32_t copy=0;copy<2;copy++) {
  uint32_t lba=v.fat+copy*v.fat_size+c/128; int r=rd(lba,s); if(r) return r;
  uint8_t *e=s+(c%128)*4; put32(e,(u32(e)&0xf0000000u)|(val&0x0fffffffu));
  if((r=wr(lba,s,0))) return r;
 }
 return 0;
}
/* A file's whole chain (at most max clusters) into out; *count clusters. */
static int walk(uint32_t first,uint32_t bytes,uint32_t *out,uint32_t max,uint32_t *count) {
 uint32_t need=(bytes+v.spc*512-1)/(v.spc*512),c=first; *count=0;
 if(!need) return 0;
 if(need>max) return fail("PUT OLD TOO BIG",27);
 for(uint32_t i=0;i<need;i++) {
  if(!cluster_ok(c)) return fail("PUT OLD CHAIN",5);
  out[(*count)++]=c; uint32_t n; int r=fat_get(c,&n); if(r) return r;
  if(i+1==need) return n>=0x0ffffff8u?0:fail("PUT OLD CHAIN",5);
  c=n;
 }
 return 0;
}
struct slot {uint32_t lba,off; bool found;};
/* Look for name in a directory chain; also remember the first usable free slot. */
static int scan(uint32_t dir,const uint8_t name[11],struct slot *hit,struct slot *free_slot,uint8_t entry[32]) {
 hit->found=free_slot->found=false; uint32_t c=dir;
 for(uint32_t hop=0;hop<65536/(v.spc*16)+1;hop++) {
  if(!cluster_ok(c)) return fail("PUT DIR CLUSTER",5);
  for(uint32_t i=0;i<v.spc;i++) {
   int r=rd(lba_of(c,i),s); if(r) return r;
   for(uint32_t o=0;o<512;o+=32) {
    uint8_t *e=s+o;
    if(!e[0]) { /* end marker: usable when the entry after it is also free or does not exist */
     bool next_free=o+32<512?!e[32]:true;
     if(!free_slot->found&&next_free) *free_slot=(struct slot){lba_of(c,i),o,true};
     return 0;
    }
    if(e[0]==0xe5) {if(!free_slot->found) *free_slot=(struct slot){lba_of(c,i),o,true};continue;}
    if((e[11]&0x3f)==0x0f) continue;
    if(!memcmp(e,name,11)) {*hit=(struct slot){lba_of(c,i),o,true};memcpy(entry,e,32);return 0;}
   }
  }
  uint32_t n; int r=fat_get(c,&n); if(r) return r;
  if(n>=0x0ffffff8u) return 0;
  c=n;
 }
 return fail("PUT DIR LOOP",5);
}
static int alloc(uint32_t n) {
 uint32_t got=0;
 for(uint32_t c=2;c<v.clusters+2&&got<n;c++) {uint32_t x; int r=fat_get(c,&x); if(r) return r; if(!x) chain[got++]=c; if(!(c&0xfff)) watchdog_feed();}
 if(got<n) return fail("PUT FULL",28);
 taken+=n; return 0;
}
static int link(uint32_t n) {
 for(uint32_t i=0;i<n;i++) {int r=fat_set(chain[i],i+1<n?chain[i+1]:0x0fffffffu); if(r) return r;}
 return 0;
}
static int write_entry(struct slot at,const uint8_t name[11],uint8_t attr,uint32_t first,uint32_t bytes) {
 int r=rd(at.lba,s); if(r) return r;
 uint8_t *e=s+at.off; memset(e,0,32); memcpy(e,name,11); e[11]=attr;
 put16(e+14,0); put16(e+16,(46u<<9)|(1u<<5)|1u); put16(e+18,(46u<<9)|(1u<<5)|1u); /* 2026-01-01 */
 put16(e+22,0); put16(e+24,(46u<<9)|(1u<<5)|1u);
 put16(e+20,first>>16); put16(e+26,first&0xffffu); put32(e+28,bytes);
 return wr(at.lba,s,0);
}
int fatput_name(const char *n,uint8_t out[11]) {
 memset(out,' ',11); unsigned k=0,base=0,ext=0; bool dot=false;
 for(const char *p=n;*p;p++) {
  char c=*p;
  if(c=='.') {if(dot||!base) return fail("PUT NAME",22);dot=true;k=8;continue;}
  if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_'||c=='-')) return fail("PUT NAME",22);
  if(dot?++ext>3:++base>8) return fail("PUT NAME",22);
  out[k++]=(uint8_t)c;
 }
 return base&&(!dot||ext)?0:fail("PUT NAME",22);
}
int fatput(const char *name,const uint8_t *data,uint32_t bytes) {
 uint8_t nm[11],e[32]; static const uint8_t os_name[11]={'O','S',' ',' ',' ',' ',' ',' ',' ',' ',' '};
 int r=fatput_name(name,nm); if(r) return r;
 if(bytes>FATPUT_MAX) return fail("PUT SIZE",27);
 if((r=asset_volume(&v))) return fail("PUT UNMOUNTED",19);
 taken=0; fc_sec=~0u;
 if((r=rd(v.base,s))) return r;
 uint32_t fsinfo=v.base+u16(s+48); if(u16(s+48)==0||u16(s+48)==0xffff) fsinfo=0;
 struct slot hit,free_slot; uint32_t dir;
 if((r=scan(v.root,os_name,&hit,&free_slot,e))) return r;
 if(hit.found) {
  if((e[11]&0x1e)!=0x10) return fail("PUT OS NOT DIR",20); /* a plain directory: not hidden, system or a label */
  dir=((uint32_t)u16(e+20)<<16)|u16(e+26); if(!cluster_ok(dir)) return fail("PUT OS CLUSTER",5);
 } else { /* create /OS: one zeroed cluster with "." and "..", then its root entry */
  if(!free_slot.found) return fail("PUT ROOT FULL",28);
  if((r=alloc(1))) return r;
  dir=chain[0];
  memset(s,0,512);
  for(uint32_t i=1;i<v.spc;i++) if((r=wr(lba_of(dir,i),s,0))) return r;
  uint8_t d[512]; memset(d,0,512); memset(d,' ',11); d[0]='.'; d[11]=0x10; put16(d+20,dir>>16); put16(d+26,dir&0xffffu);
  memset(d+32,' ',11); d[32]='.'; d[33]='.'; d[43]=0x10; /* parent is the root: cluster 0 */
  if((r=wr(lba_of(dir,0),d,0))) return r;
  if((r=link(1))) return r;
  if((r=write_entry(free_slot,os_name,0x10,dir,0))) return r;
 }
 if((r=scan(dir,nm,&hit,&free_slot,e))) return r;
 uint32_t olds=0;
 if(hit.found) {
  if(e[11]&0x1f) return fail("PUT NOT FILE",21); /* read-only, hidden, system, label or directory */
  uint32_t first=((uint32_t)u16(e+20)<<16)|u16(e+26);
  if((r=walk(first,u32(e+28),old,sizeof old/4,&olds))) return r;
 } else if(!free_slot.found) return fail("PUT DIR FULL",28);
 uint32_t cb=v.spc*512,n=(bytes+cb-1)/cb;
 if((r=alloc(n))) return r;
 for(uint32_t i=0;i<n;i++) for(uint32_t j=0;j<v.spc;j++) {
  uint32_t at=i*cb+j*512; memset(s,0,512); if(at<bytes) memcpy(s,data+at,bytes-at<512?bytes-at:512);
  uint8_t copy[512]; memcpy(copy,s,512); if((r=wr(lba_of(chain[i],j),copy,0))) return r;
  if(!(j&15)) watchdog_feed();
 }
 if((r=link(n))) return r;
 if((r=write_entry(hit.found?hit:free_slot,nm,0x20,n?chain[0]:0,bytes))) return r;
 for(uint32_t i=0;i<olds;i++) if((r=fat_set(old[i],0))) return r;
 if(fsinfo&&!rd(fsinfo,s)&&u32(s)==0x41615252u&&u32(s+484)==0x61417272u) {
  uint32_t f=u32(s+488),nf=f-taken+olds;
  if(f<=v.clusters&&f>=taken&&nf<=v.clusters) {put32(s+488,nf);if((r=wr(fsinfo,s,fsinfo))) return r;}
  else if(f!=~0u) {put32(s+488,~0u);if((r=wr(fsinfo,s,fsinfo))) return r;} /* it was wrong: say unknown */
 }
 fatput_fail="none"; return asset_mount();
}
