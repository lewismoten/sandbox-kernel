/* SPDX-License-Identifier: MIT; original read-only FAT32 asset reader.
 * Validated VFAT names plus the original 8.3 aliases. Every
 * loop is bounded by cluster or entry counts; corrupt metadata fails with
 * -EIO and a code in asset_fail, never a hang or an out-of-volume read. */
#include "kernel.h"
#include "assets.h"
#include "vfat.h"
const char *asset_fail="none";
static struct {uint32_t base,end,fat,fat_size,data,clusters,spc,root;bool mounted;} v;
struct cursor {uint32_t cluster,index,hops,limit;bool done;};
/* Directory scratch is invocation-local: find/readdir, or ONE checked
 * call20 batch. It is never stored in an asset handle or global cache. */
static struct handle {
 bool used,dir,cache; uint16_t gen; int err;
 uint32_t first,size,pos,cluster;
 struct cursor c;
} handles[ASSET_HANDLES];
static uint8_t fat_buf[512],buf[512]; static uint32_t fat_lba=~0u;
static uint16_t u16(const uint8_t *p) {return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t u32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
static int fail(const char *why,int e) {asset_fail=why;return -e;}
static uint16_t bump(uint16_t g) {return (uint16_t)(g==0xffffu?1:g+1);} /* generation 0 is never issued */
static bool cluster_ok(uint32_t c) {return c>=2&&c<v.clusters+2;}
static uint32_t cluster_bytes(void) {return v.spc*512;}
static int rd(uint32_t lba,uint8_t *b) {
 if(lba<v.base||lba>=v.end) return fail("ASSET LBA",ASSET_EIO);
 return sd_read(lba,b)?0:fail("ASSET SD READ",ASSET_EIO);
}
/* 1: end of chain, 0: *next is the next cluster, <0: corrupt or I/O. */
static int next_cluster(uint32_t c,uint32_t *next) {
 if(!cluster_ok(c)) return fail("FAT CLUSTER",ASSET_EIO);
 uint32_t lba=v.fat+c/128;
 if(lba!=fat_lba) {fat_lba=~0u;int r=rd(lba,fat_buf);if(r)return r;fat_lba=lba;}
 uint32_t n=u32(fat_buf+(c%128)*4)&0x0fffffffu;
 if(n>=0x0ffffff8u) return 1;
 if(!cluster_ok(n)) return fail(n?"FAT BAD":"FAT FREE",ASSET_EIO);
 *next=n; return 0;
}
int asset_mount(void) {
 for(unsigned i=0;i<ASSET_HANDLES;i++) if(handles[i].used) {handles[i].used=false;handles[i].gen=bump(handles[i].gen);}
 v.mounted=false;fat_lba=~0u;v.base=0;v.end=sd_sectors();
 uint8_t *s=buf; int r;
 if((r=rd(0,s))) return r;
 if(u16(s+510)!=0xaa55) return fail("ASSET MBR SIG",ASSET_EIO);
 if(s[450]!=0x0b&&s[450]!=0x0c) return fail("ASSET MBR TYPE",ASSET_ENODEV);
 uint32_t base=u32(s+454),count=u32(s+458);
 if(!base||count<65536||base>=v.end||count>v.end-base) return fail("ASSET MBR RANGE",ASSET_EIO);
 v.base=base;v.end=base+count;
 if((r=rd(base,s))) return r;
 if(u16(s+510)!=0xaa55||u16(s+11)!=512||s[16]!=2||u16(s+17)||u16(s+19)||u16(s+22)||u16(s+42)) return fail("ASSET BPB",ASSET_EIO);
 uint32_t spc=s[13],reserved=u16(s+14),fat_size=u32(s+36),total=u32(s+32),root=u32(s+44);
 if(!spc||spc>128||(spc&(spc-1))||!reserved||!fat_size||total>count||total<65536||(uint64_t)reserved+2ull*fat_size>=total) return fail("ASSET BPB GEOM",ASSET_EIO);
 v.end=base+total;v.fat=base+reserved;v.fat_size=fat_size;v.data=v.fat+2*fat_size;v.spc=spc;
 v.clusters=(v.end-v.data)/spc;
 if(v.clusters<65525||v.clusters>0x0ffffff5u||(uint64_t)fat_size*128<(uint64_t)v.clusters+2) return fail("ASSET BPB CLUSTERS",ASSET_EIO);
 if(!cluster_ok(root)) return fail("ASSET ROOT",ASSET_EIO);
 v.root=root;v.mounted=true;asset_fail="none";return 0;
}
static void cursor_start(struct cursor *c,uint32_t first) {
 c->cluster=first;c->index=0;c->hops=0;c->done=false;
 c->limit=65536/(v.spc*16)+1; /* FAT allows at most 65536 entries per directory */
}
/* Next raw in-use short entry: 1 with e filled, 0 at end, <0 on error. */
static int cursor_next(struct cursor *c,uint8_t e[32],struct asset_dir_sector *s,char name[256]) {
 struct cursor saved=*c;struct vfat_name lfn;vfat_reset(&lfn);name[0]=0;
 for(;;) {
  if(c->done) return 0;
  if(c->index==v.spc*16) {
   uint32_t n; int r=next_cluster(c->cluster,&n);
   if(r<0) {*c=saved;return r;}
   if(r) {c->done=true;return 0;}
   if(++c->hops>=c->limit) return fail("DIR LOOP",ASSET_EIO);
   c->cluster=n;c->index=0;
  }
  if(!cluster_ok(c->cluster)) return fail("DIR CLUSTER",ASSET_EIO);
  uint32_t lba=v.data+(c->cluster-2)*v.spc+c->index/16;
  if(s->lba!=lba) {
   s->lba=~0u;int r=rd(lba,s->bytes);if(r){*c=saved;return r;}s->lba=lba;
  }
  memcpy(e,s->bytes+(c->index%16)*32,32); c->index++;
  if(!e[0]) {c->done=true;return 0;}
  if(e[0]==0xe5) {vfat_reset(&lfn);continue;}
  if((e[11]&0x3f)==0x0f) {vfat_slot(&lfn,e);continue;}
  if(e[11]&0x08) {vfat_reset(&lfn);continue;}
  if(e[11]&0xc0) return fail("DIR ATTR",ASSET_EIO);
  for(unsigned i=0;i<11;i++) if(e[i]<0x20&&!(i==0&&e[0]==0x05)) return fail("DIR NAME",ASSET_EIO);
  if(!vfat_decode(&lfn,e,name))name[0]=0;
  if(e[0]==0x05) e[0]=0xe5;
  return 1;
 }
}
/* Path component to the padded 11-byte short form. 0, -EINVAL or -ENOENT
   (a name only a long-name entry could hold). */
static int short_name(const char *p,uint32_t n,uint8_t out[11]) {
 static const char bad[]="\"*+,/:;<=>?[\\]|";
 if(!n) return fail("PATH EMPTY",ASSET_EINVAL);
 if((n==1&&p[0]=='.')||(n==2&&p[0]=='.'&&p[1]=='.')) return fail("PATH DOT",ASSET_EINVAL);
 memset(out,' ',11); uint32_t k=0,base=0,ext=0; bool dot=false;
 for(uint32_t i=0;i<n;i++) {
  uint8_t ch=(uint8_t)p[i];
  if(ch<0x20||ch==0x7f) return fail("PATH CHAR",ASSET_EINVAL);
  for(const char *b=bad;*b;b++) if(ch==(uint8_t)*b) return fail("PATH CHAR",ASSET_EINVAL);
  if(ch>=0x80||ch==' ') return fail("PATH LONG NAME",ASSET_ENOENT);
  if(ch=='.') {if(dot||!base) return fail("PATH LONG NAME",ASSET_ENOENT);dot=true;k=8;continue;}
  if(ch>='a'&&ch<='z') ch=(uint8_t)(ch-32);
  if(dot) {if(++ext>3) return fail("PATH LONG NAME",ASSET_ENOENT);}
  else if(++base>8) return fail("PATH LONG NAME",ASSET_ENOENT);
  out[k++]=ch;
 }
 if(dot&&!ext) return fail("PATH LONG NAME",ASSET_ENOENT);
 if(out[0]==0xe5) out[0]=0x05;
 return 0;
}
static uint32_t entry_cluster(const uint8_t *e) {return ((uint32_t)u16(e+20)<<16)|u16(e+26);}
static int find(uint32_t dir,const char *path,uint32_t bytes,uint8_t e[32]) {
 struct cursor c; cursor_start(&c,dir);
 struct asset_dir_sector s;s.lba=~0u;
 uint8_t want[11];bool short_ok=short_name(path,bytes,want)==0;
 if(short_ok&&want[0]==0x05)want[0]=0xe5;
 char name[256];uint8_t candidate[32];bool found=false;
 for(;;) {
  int r=cursor_next(&c,e,&s,name);
  if(r<0)return r;
  if(!r) {if(found){memcpy(e,candidate,32);return 0;}return fail("ASSET NOT FOUND",ASSET_ENOENT);}
  if(short_ok&&!memcmp(e,want,11))return 0; /* old aliases cannot be shadowed by an LFN */
  if(!found&&name[0]&&vfat_equal(path,bytes,name)) {
   if(!short_ok)return 0;
   memcpy(candidate,e,32);found=true;
  }
 }
}
static int new_handle(void) {
 for(unsigned i=0;i<ASSET_HANDLES;i++) if(!handles[i].used) {
  struct handle *h=&handles[i]; uint16_t g=(uint16_t)(h->gen?h->gen:1); memset(h,0,sizeof(*h)); h->gen=g; h->used=true;
  return (int)(((uint32_t)g<<3)|i);
 }
 return fail("ASSET EMFILE",ASSET_EMFILE);
}
static struct handle *get(int id) {
 if(id<0) return NULL;
 struct handle *h=&handles[id&7];
 return h->used&&h->gen==(uint16_t)((uint32_t)id>>3)&&((uint32_t)id>>3)<=0xffffu?h:NULL;
}
int asset_open(const char *path,uint32_t n,uint32_t flags) {
 if(!v.mounted) return fail("ASSET UNMOUNTED",ASSET_ENODEV);
 if(flags&~3u) return fail("ASSET FLAGS",ASSET_EINVAL);
 if(flags) return fail("ASSET READ ONLY",ASSET_EROFS);
 if(!path||!n) return fail("PATH EMPTY",ASSET_EINVAL);
 if(n>ASSET_MAX_PATH) return fail("PATH TOO LONG",ASSET_ENAMETOOLONG);
 if(path[0]!='/') return fail("PATH RELATIVE",ASSET_EINVAL);
 for(uint32_t i=0;i<n;i++) if(!path[i]) return fail("PATH NUL",ASSET_EINVAL);
 {uint32_t parts=0; for(uint32_t i=0;i<n;i++) if(path[i]=='/') parts++;
  if(parts>ASSET_MAX_DEPTH+1) return fail("PATH TOO DEEP",ASSET_ENAMETOOLONG);}
 fat_lba=~0u; /* never trust a cached FAT sector across calls */
 uint32_t cluster=v.root,size=0; bool dir=true,os_dir=false,cache=false; uint32_t at=1,depth=0;
 bool trailing=n>1&&path[n-1]=='/'; /* one trailing slash names a directory */
 if(trailing) {n--; if(path[n-1]=='/') return fail("PATH EMPTY",ASSET_EINVAL);}
 while(at<n) {
  if(!dir) return fail("PATH NOT DIR",ASSET_ENOTDIR);
  if(++depth>ASSET_MAX_DEPTH) return fail("PATH TOO DEEP",ASSET_ENAMETOOLONG); /* not reached after the count above */
  uint32_t end=at; while(end<n&&path[end]!='/') end++;
  uint8_t e[32];int r;
  if(!vfat_component(path+at,end-at))return fail("PATH CHAR",ASSET_EINVAL);
  if((r=find(cluster,path+at,end-at,e))) return r;
  if(depth==1)os_dir=!memcmp(e,"OS         ",11);
  if(depth==2)cache=os_dir&&(!memcmp(e,"CACHE      ",11)||!memcmp(e,"SAVES      ",11)); /* both are writable by apps: reads block mutation */
  dir=(e[11]&0x10)!=0; cluster=entry_cluster(e); size=u32(e+28);
  if(dir) {
   if(!cluster_ok(cluster)) /* cluster 0 is only valid in "..", never reached by a path */ return fail("DIR ENTRY CLUSTER",ASSET_EIO);
  } else {
   if(size>ASSET_MAX_BYTES) return fail("FILE TOO BIG",ASSET_EIO);
   if(size&&!cluster_ok(cluster)) return fail("FILE CLUSTER",ASSET_EIO);
  }
  at=end+1;
 }
 if(!dir&&trailing) return fail("PATH NOT DIR",ASSET_ENOTDIR);
 if(!dir&&size) { /* the chain must hold exactly the clusters the size needs */
  uint32_t need=(size+cluster_bytes()-1)/cluster_bytes(),c=cluster,next=0;
  for(uint32_t i=1;i<need;i++) {int r=next_cluster(c,&next); if(r<0) return r; if(r) return fail("FAT SHORT",ASSET_EIO); c=next;}
  int r=next_cluster(c,&next); if(r<0) return r; if(!r) return fail("FAT LONG",ASSET_EIO);
 }
 int id=new_handle(); if(id<0) return id;
 struct handle *h=get(id); h->dir=dir; h->first=cluster; h->size=dir?0:size; h->cluster=cluster;
 h->cache=cache; /* identify the controlled short entries, not the chosen LFN spelling */
 if(dir) cursor_start(&h->c,cluster);
 return id;
}
int asset_seek(int id,uint64_t offset) {
 struct handle *h=get(id); if(!h) return fail("ASSET BADF",ASSET_EBADF);
 if(h->dir) return fail("ASSET NOT SEEKABLE",ASSET_ESPIPE);
 if(offset>h->size) return fail("ASSET OFFSET",ASSET_EINVAL);
 /* read keeps the cluster containing pos-1 (or first at zero), including
    exact cluster boundaries and EOF. Build a candidate before committing. */
 uint32_t pos=(uint32_t)offset,cb=cluster_bytes();
 uint32_t target=pos?(pos-1)/cb:0,current=h->pos?(h->pos-1)/cb:0;
 uint32_t c=h->cluster,at=current;
 if(target<current) {c=h->first;at=0;}
 fat_lba=~0u;
 for(;at<target;at++) {
  uint32_t next; int r=next_cluster(c,&next);
  if(r<0) return r;
  if(r) return fail("FAT SHORT",ASSET_EIO);
  c=next;
 }
 h->cluster=c;h->pos=pos;h->err=0;return 0;
}
int asset_read(int id,void *out,uint32_t count) {
 struct handle *h=get(id); if(!h) return fail("ASSET BADF",ASSET_EBADF);
 if(h->dir) return fail("ASSET IS DIR",ASSET_EINVAL);
 if(count>ASSET_MAX_READ) return fail("ASSET COUNT",ASSET_EINVAL);
 if(!out&&count) return fail("ASSET BUFFER",ASSET_EINVAL);
 if(h->err) {int e=h->err;h->err=0;return -e;}
 if(h->pos>=h->size||!count) return 0;
 if(count>h->size-h->pos) count=h->size-h->pos;
 fat_lba=~0u; uint8_t *dst=out; uint32_t done=0,cb=cluster_bytes();
 while(done<count) {
  uint32_t cluster=h->cluster;
  if(h->pos&&h->pos%cb==0&&h->pos!=h->size) {
   /* h->cluster still names the cluster that held the previous byte */
   uint32_t n; int r=next_cluster(cluster,&n); if(!r) cluster=n; else {r=r<0?-r:(asset_fail="FAT SHORT",ASSET_EIO); if(done){h->err=r;return (int)done;} return -r;}
  }
  uint32_t in=h->pos%cb,off=in%512,take=512-off; if(take>count-done) take=count-done;
  int r=rd(v.data+(cluster-2)*v.spc+in/512,buf); if(r) {if(done){h->err=-r;return (int)done;}return r;}
  h->cluster=cluster; /* failed data I/O must not get ahead of pos-1 */
  memcpy(dst+done,buf+off,take); done+=take; h->pos+=take;
  /* keep h->cluster on the cluster holding pos-1 until pos crosses into the next */
 }
 return (int)done;
}
static void format_name(const uint8_t *e,char out[13]) {
 unsigned k=0; for(unsigned i=0;i<8&&e[i]!=' ';i++) out[k++]=(char)e[i];
 if(e[8]!=' ') {out[k++]='.';for(unsigned i=8;i<11&&e[i]!=' ';i++) out[k++]=(char)e[i];}
 out[k]=0;
}
int asset_readdir_long_batch(int id,struct asset_long_dirent *out,uint32_t meta[3],struct asset_dir_sector *s) {
 struct handle *h=get(id); if(!h) return fail("ASSET BADF",ASSET_EBADF);
 if(!h->dir) return fail("ASSET NOT DIR",ASSET_ENOTDIR);
 if(!out) return fail("ASSET BUFFER",ASSET_EINVAL);
 struct asset_dirent *d=&out->short_entry;
 fat_lba=~0u; uint8_t e[32];
 for(;;) {
  char name[256];int r=cursor_next(&h->c,e,s,name); if(r<=0) return r;
  if(e[0]=='.') continue; /* "." and ".." */
  memset(d,0,sizeof(*d)); format_name(e,d->name); d->attr=e[11]; d->is_dir=(e[11]&0x10)?1:0; d->size=d->is_dir?0:u32(e+28);
  memset(out->name,0,sizeof out->name);
  memcpy(out->name,name[0]?name:d->name,strlen(name[0]?name:d->name));
  if(meta) {meta[0]=u32(e+22);meta[1]=u32(e+14);meta[2]=entry_cluster(e);}
  return 1;
 }
}
int asset_readdir_long(int id,struct asset_long_dirent *out,uint32_t meta[3]) {
 struct asset_dir_sector s;s.lba=~0u;
 return asset_readdir_long_batch(id,out,meta,&s);
}
int asset_readdir_meta(int id,struct asset_dirent *d,uint32_t meta[3]) {
 if(!d)return asset_readdir_long(id,NULL,meta); /* preserve BADF/NOTDIR before buffer validation */
 struct asset_long_dirent out;int r=asset_readdir_long(id,&out,meta);if(r==1)*d=out.short_entry;return r;
}
int asset_readdir(int id,struct asset_dirent *d) {return asset_readdir_meta(id,d,NULL);}
int asset_size(int id,uint32_t *bytes) {struct handle *h=get(id); if(!h) return fail("ASSET BADF",ASSET_EBADF); *bytes=h->size; return 0;}
int asset_cache_readers(void) {int n=0;for(unsigned i=0;i<ASSET_HANDLES;i++)n+=handles[i].used&&handles[i].cache;return n;}
int asset_close(int id) {
 struct handle *h=get(id); if(!h) return fail("ASSET BADF",ASSET_EBADF);
 h->used=false; h->gen=bump(h->gen); return 0;
}
int asset_volume(struct asset_volume *o) {
 if(!v.mounted) return fail("ASSET UNMOUNTED",ASSET_ENODEV);
 o->base=v.base;o->end=v.end;o->fat=v.fat;o->fat_size=v.fat_size;o->data=v.data;o->clusters=v.clusters;o->spc=v.spc;o->root=v.root;return 0;
}
