/* SPDX-License-Identifier: MIT; original restricted FAT32 data-sector writer.
 * No allocation, renaming, truncation, FAT or directory writes. */
#include "kernel.h"
const char *store_fail="none";
struct file_extent slots[2],controls[2];
uint32_t control[128];
static unsigned current_copy;
static uint32_t base,end,fat,data,clusters,fat_size,spc;
static uint8_t sector[512];
static uint16_t u16(const uint8_t *p) { return p[0]|(p[1]<<8); }
static uint32_t u32(const uint8_t *p) { return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24); }
static bool cluster_ok(uint32_t c) { return c>=2 && c<clusters+2; }
static bool next_cluster(uint32_t c,uint32_t *out) {
 if(!cluster_ok(c)) return false;
 uint32_t sec=(c*4)/512,off=(c*4)%512; if(sec>=fat_size||!sd_read(fat+sec,sector)) return false;
 uint32_t a=u32(sector+off)&0x0fffffff;
 if(!sd_read(fat+fat_size+sec,sector) || (u32(sector+off)&0x0fffffff)!=a) return false;
 *out=a; return true;
}
static bool map_file(struct file_extent *f,uint32_t c,uint32_t bytes) {
 if(!bytes||bytes%512||bytes>MAX_IMAGE) return false;
 f->bytes=bytes; f->count=0;
 while(f->count<bytes/512) {
  if(!cluster_ok(c)) return false;
  for(unsigned i=0;i<spc && f->count<bytes/512;i++) f->sectors[f->count++]=data+(c-2)*spc+i;
  uint32_t next; if(!next_cluster(c,&next)) return false;
  if(f->count==bytes/512) return next>=0xffffff8;
  c=next;
 }
 return false;
}
static bool record_valid(const uint32_t *c) {
 if(c[0]!=CONTROL_MAGIC||c[1]!=1||!c[2]||c[2]==~0u||c[3]>1|| (c[4]>1&&c[4]!=NO_SLOT)||c[5]>1||c[8]!=NO_SLOT||!c[6]) return false;
 if(c[4]!=NO_SLOT&&(c[4]==c[3]||c[7]<=c[6])) return false;
 if(c[4]==NO_SLOT) {if(c[5]||c[7])return false;for(unsigned i=24;i<32;i++)if(c[i])return false;}
 for(unsigned i=9;i<16;i++) if(c[i]) return false;
 for(unsigned i=32;i<127;i++) if(c[i]) return false;
 return c[127]==crc32(c,508);
}
bool store_init(void) {
 store_fail="none"; memset(slots,0,sizeof(slots)); memset(controls,0,sizeof(controls)); memset(control,0,sizeof(control));
 if(!sd_init()) {store_fail=sd_fail;return false;}
 if(!sd_read(0,sector)) {store_fail="SD READ0";return false;}
 if(u16(sector+510)!=0xaa55) {store_fail="MBR SIG";return false;}
 if(sector[450]!=0x0c && sector[450]!=0x0b) {store_fail="MBR TYPE";return false;}
 base=u32(sector+454); uint32_t count=u32(sector+458);
 if(base<2048||count<262144||base>=sd_sectors()||count>sd_sectors()-base) {store_fail="MBR RANGE";return false;} end=base+count;
 if(!sd_read(base,sector)) {store_fail="SD READ BPB";return false;}
 if(u16(sector+510)!=0xaa55||u16(sector+11)!=512||sector[16]!=2||u16(sector+17)||u16(sector+22)||u16(sector+42)) {store_fail="BPB";return false;}
 spc=sector[13]; fat_size=u32(sector+36); uint32_t reserved=u16(sector+14),total=u32(sector+32),root=u32(sector+44);
 if(!spc||spc>128||(spc&(spc-1))||reserved<1||!fat_size||fat_size>count/4||u16(sector+40)||total!=count||reserved+fat_size*2>=count) {store_fail="BPB GEOM";return false;}
 fat=base+reserved; data=fat+fat_size*2; clusters=(end-data)/spc;
 if(clusters<65525||clusters>0x0fffffed||fat_size<((clusters+2)*4+511)/512) {store_fail="BPB CLUSTERS";return false;}
 const char names[4][12]={"KERNA   BIN","KERNB   BIN","BOOTCT0 BIN","BOOTCT1 BIN"};
 uint32_t starts[4]={0},sizes[4]={0},root_first=root; bool done=false;
 for(unsigned hop=0;hop<128&&!done;hop++) {
  if(!cluster_ok(root)) {store_fail="DIR CLUSTER";return false;};
  for(unsigned j=0;j<spc&&!done;j++) {
   if(!sd_read(data+(root-2)*spc+j,sector)) {store_fail="DIR READ";return false;};
   for(unsigned i=0;i<512;i+=32) {
    if(!sector[i]) {done=true;break;} if(sector[i]==0xe5||sector[i+11]==15) continue;
    for(unsigned n=0;n<4;n++) if(!memcmp(sector+i,names[n],11)) {
     if(starts[n]||(sector[i+11]&0x18)) {store_fail="DIR DUP";return false;};
     starts[n]=((uint32_t)u16(sector+i+20)<<16)|u16(sector+i+26); sizes[n]=u32(sector+i+28);
    }
   }
  }
  if(!done) { uint32_t n; if(!next_cluster(root,&n)) {store_fail="DIR FAT";return false;}; if(n>=0xffffff8) done=true; else root=n; }
 }
 if(!done) {store_fail="DIR END";return false;};
 for(unsigned i=0;i<4;i++) { struct file_extent *f=i<2?&slots[i]:&controls[i-2]; if(!map_file(f,starts[i],sizes[i])) {store_fail="FILE MAP";return false;}; if(i<2&&sizes[i]!=MAX_IMAGE) {store_fail="FILE SIZE";return false;}; if(i>=2&&sizes[i]!=4096) {store_fail="FILE SIZE";return false;}; }
 /* Reject overlapping/repeated clusters even on malicious FAT chains. Sorted
    global sector list also detects loops within each individual file. */
 static uint32_t all[8210]; unsigned num=0;
 for(unsigned i=0;i<4;i++) { struct file_extent *f=i<2?&slots[i]:&controls[i-2]; for(unsigned j=0;j<f->count;j++) all[num++]=f->sectors[j]; }
 /* Shell sort: bounded O(n log^2 n), no dynamic allocation. */
 for(unsigned gap=num/2;gap;gap/=2) for(unsigned i=gap;i<num;i++) { uint32_t v=all[i]; unsigned j=i; while(j>=gap&&all[j-gap]>v) {all[j]=all[j-gap];j-=gap;} all[j]=v; }
 for(unsigned i=1;i<num;i++) if(all[i]==all[i-1]) {store_fail="FILE OVERLAP";return false;};
 /* Directory clusters are never writable slot/control storage. Check the
    complete root chain, including clusters after the first unused entry. */
 root=root_first;done=false;
 for(unsigned hop=0;hop<128;hop++) {
  if(!cluster_ok(root)){store_fail="DIR CLUSTER2";return false;};
  uint32_t first=data+(root-2)*spc;unsigned lo=0,hi=num;
  while(lo<hi) {unsigned mid=lo+(hi-lo)/2;if(all[mid]<first)lo=mid+1;else hi=mid;}
  if(lo<num&&all[lo]<first+spc){store_fail="DIR OVERLAP";return false;};
  uint32_t next;if(!next_cluster(root,&next)){store_fail="DIR FAT2";return false;};
  if(next>=0xffffff8) {done=true;break;}root=next;
 }
 if(!done){store_fail="DIR END2";return false;};
 uint32_t a=controls[0].sectors[0],b=controls[1].sectors[0]; if((a>b?a-b:b-a)<8) {store_fail="CTRL SPACING";return false;};
 uint32_t records[2][128]; bool ok[2];
 for(unsigned i=0;i<2;i++) ok[i]=file_read(&controls[i],0,records[i])&&record_valid(records[i]);
 if(!ok[0]&&!ok[1]) return true; /* selector may recover from valid slot */
 if(ok[0]&&ok[1]&&records[0][2]==records[1][2]&&memcmp(records[0],records[1],512)) {store_fail="CTRL CONFLICT";return false;};
 current_copy=ok[1]&&(!ok[0]||records[1][2]>records[0][2])?1:0;
 memcpy(control,records[current_copy],512); return true;
}
bool file_read(const struct file_extent *f,uint32_t s,void *p) { return s<f->count&&sd_read(f->sectors[s],p); }
bool file_write(const struct file_extent *f,uint32_t s,const void *p) { return s<f->count&&sd_write(f->sectors[s],p); }
bool control_commit(const uint32_t next[128]) {
 if(!control[2]||control[2]>=0xfffffffeu) return false;
 uint32_t copy[128],check[128]; memcpy(copy,next,512); copy[2]=control[2]+1; copy[127]=crc32(copy,508);
 if(!record_valid(copy)) return false;
 unsigned target=1-current_copy;
 if(!file_write(&controls[target],0,copy)||!file_read(&controls[target],0,check)||memcmp(copy,check,512)) return false;
 memcpy(control,copy,512); current_copy=target; return true;
}
bool slot_load(unsigned slot,uint8_t *dest,uint32_t *bytes) {
 if(slot>1||!file_read(&slots[slot],0,sector)) return false;
 uint32_t size=u32(sector+16); if(size<4||size>MAX_IMAGE-256) return false; size+=256;
 uint8_t temp[512];
 for(unsigned i=0;i<(size+511)/512;i++) { if(!file_read(&slots[slot],i,temp)) return false; unsigned n=size-i*512; if(n>512)n=512; memcpy(dest+i*512,temp,n); }
 if(!image_valid(dest,size)) return false;
 if(control[0]==CONTROL_MAGIC) {
  uint8_t hash[32]; sha256(dest,size,hash);
  const uint32_t *expected=slot==control[3]?control+16:slot==control[4]?control+24:NULL;
  if(!expected||memcmp(hash,expected,32)) return false;
 }
 *bytes=size; return true;
}

bool control_healthy(void) {uint32_t c[128];return file_read(&controls[current_copy],0,c)&&record_valid(c)&&!memcmp(c,control,512);}
