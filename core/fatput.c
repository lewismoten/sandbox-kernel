/* SPDX-License-Identifier: MIT; original restricted FAT32 writer for /OS,
 * /OS/APPS/<DIR> and /GAMES/<DIR> (development installs over USB PUTF).
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
/* A cut between the two copies of one FAT write leaves them different. Changing commands
   heal that entry (LS never writes): to the copy that is not free, copy 0 if both are not
   (what readers already follow), so healing can leak a cluster but never free one. */
static bool heal;
static int fat_get(uint32_t c,uint32_t *out) {
 uint32_t sec=c/128; int r;
 if(sec>=v.fat_size) return fail("PUT FAT RANGE",5);
 if(sec!=fc_sec) {fc_sec=~0u; if((r=rd(v.fat+sec,fc[0]))||(r=rd(v.fat+v.fat_size+sec,fc[1]))) return r; fc_sec=sec;}
 uint8_t *e0=fc[0]+(c%128)*4,*e1=fc[1]+(c%128)*4; uint32_t a=u32(e0)&0x0fffffffu,b=u32(e1)&0x0fffffffu;
 if(b!=a) {
  if(!heal) return fail("PUT FAT MISMATCH",5);
  uint32_t k=a?1:0; uint8_t *e=k?e1:e0; if(!a) a=b;
  put32(e,(u32(e)&0xf0000000u)|a);
  if((r=wr(v.fat+k*v.fat_size+sec,fc[k],0))) {fc_sec=~0u;return r;}
 }
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
static uint32_t scan_last,scan_hops; /* the last cluster scan() saw, and how many */
/* The valid long-name group right before the entry scan() last found: its slots, nearest the
   short entry last. Only a group whose order and checksum match that entry is kept. */
static struct slot lfn_at[20],lfn_run[20]; static unsigned lfn_n,lfn_run_n;
static uint8_t lfn_run_ord[20],lfn_run_sum[20];
static uint8_t lfn_sum(const uint8_t *n) {uint8_t x=0; for(unsigned i=0;i<11;i++) x=(uint8_t)(((x&1)<<7)+(x>>1)+n[i]); return x;}
static void lfn_keep(const uint8_t name[11]) {
 uint8_t sum=lfn_sum(name); lfn_n=0;
 for(unsigned k=0;k<lfn_run_n;k++) { /* k = distance from the short entry */
  unsigned j=lfn_run_n-1-k;
  if((lfn_run_ord[j]&0x1f)!=k+1||lfn_run_sum[j]!=sum) return;
  if(lfn_run_ord[j]&0x40) {for(unsigned i=0;i<=k;i++) lfn_at[i]=lfn_run[j+i];lfn_n=k+1;return;}
 }
}
/* Mark a long-name group as deleted (the short entry is handled by the caller). */
static int lfn_delete(const struct slot *g,unsigned n) {
 for(unsigned i=0;i<n;i++) {
  int r=rd(g[i].lba,s); if(r) return r;
  s[g[i].off]=0xe5; if((r=wr(g[i].lba,s,0))) return r;
 }
 return 0;
}
/* Look for name in a directory chain; also remember the first usable free slot. */
static int scan(uint32_t dir,const uint8_t name[11],struct slot *hit,struct slot *free_slot,uint8_t entry[32]) {
 hit->found=free_slot->found=false; uint32_t c=dir; lfn_n=lfn_run_n=0;
 for(uint32_t hop=0;hop<65536/(v.spc*16)+1;hop++) {
  if(!cluster_ok(c)) return fail("PUT DIR CLUSTER",5);
  scan_last=c;scan_hops=hop+1;
  for(uint32_t i=0;i<v.spc;i++) {
   int r=rd(lba_of(c,i),s); if(r) return r;
   for(uint32_t o=0;o<512;o+=32) {
    uint8_t *e=s+o;
    if(!e[0]) { /* end marker: usable when the entry after it is also free or does not exist */
     bool next_free=o+32<512?!e[32]:true;
     if(!free_slot->found&&next_free) *free_slot=(struct slot){lba_of(c,i),o,true};
     return 0;
    }
    if(e[0]==0xe5) {lfn_run_n=0;if(!free_slot->found) *free_slot=(struct slot){lba_of(c,i),o,true};continue;}
    if((e[11]&0x3f)==0x0f) {
     if(e[0]&0x40) lfn_run_n=0;
     if(lfn_run_n<20) {lfn_run[lfn_run_n]=(struct slot){lba_of(c,i),o,true};lfn_run_ord[lfn_run_n]=e[0];lfn_run_sum[lfn_run_n]=e[13];lfn_run_n++;}
     continue;
    }
    if(!memcmp(e,name,11)) {*hit=(struct slot){lba_of(c,i),o,true};memcpy(entry,e,32);lfn_keep(name);return 0;}
    lfn_run_n=0;
   }
  }
  uint32_t n; int r=fat_get(c,&n); if(r) return r;
  if(n>=0x0ffffff8u) return 0;
  c=n;
 }
 return fail("PUT DIR LOOP",5);
}
/* Real entries in a directory chain: not free, not long-name pieces, not "." or "..". */
static int count_entries(uint32_t dir,uint32_t *count) {
 uint32_t c=dir,n=0;
 for(uint32_t hop=0;hop<65536/(v.spc*16)+1;hop++) {
  if(!cluster_ok(c)) return fail("PUT DIR CLUSTER",5);
  for(uint32_t i=0;i<v.spc;i++) {
   int r=rd(lba_of(c,i),s); if(r) return r;
   for(uint32_t o=0;o<512;o+=32) {
    const uint8_t *e=s+o;
    if(!e[0]) {*count=n;return 0;}
    if(e[0]==0xe5||(e[11]&0x3f)==0x0f||e[0]=='.') continue;
    n++;
   }
  }
  uint32_t nx; int r=fat_get(c,&nx); if(r) return r;
  if(nx>=0x0ffffff8u) {*count=n;return 0;}
  c=nx;
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
/* A full directory (not the root) gains one zeroed cluster at the end of its
   chain: zeroed first, marked end-of-chain, and only then linked, so a power
   cut leaks it at worst. Its first slot is the new end marker. */
static int grow(struct slot *free_slot) {
 if((scan_hops+1)*v.spc*16>65536) return fail("PUT DIR FULL",28);
 int r=alloc(1); if(r) return r;
 uint32_t c=chain[0]; uint8_t z[512]; memset(z,0,512);
 for(uint32_t i=0;i<v.spc;i++) if((r=wr(lba_of(c,i),z,0))) return r;
 if((r=fat_set(c,0x0fffffffu))||(r=fat_set(scan_last,c))) return r;
 *free_slot=(struct slot){lba_of(c,0),0,true}; return 0;
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
/* Find the directory name11 in parent (a cluster; the root's for top level),
   or create it: one zeroed cluster with "." and "..", then its entry. */
static int ensure_dir(uint32_t parent,bool top,const uint8_t name[11],uint32_t *dir) {
 struct slot hit,free_slot; uint8_t e[32]; int r;
 if((r=scan(parent,name,&hit,&free_slot,e))) return r;
 if(hit.found) {
  if((e[11]&0x1e)!=0x10) return fail("PUT NOT DIR",20); /* a plain directory: not hidden, system or a label */
  *dir=((uint32_t)u16(e+20)<<16)|u16(e+26); return cluster_ok(*dir)?0:fail("PUT DIR CLUSTER",5);
 }
 if(!free_slot.found&&(top||(r=grow(&free_slot)))) return top?fail("PUT ROOT FULL",28):r;
 if((r=alloc(1))) return r;
 uint32_t d0=chain[0],up=top?0:parent; /* ".." of a top-level directory is cluster 0 */
 memset(s,0,512);
 for(uint32_t i=1;i<v.spc;i++) if((r=wr(lba_of(d0,i),s,0))) return r;
 uint8_t d[512]; memset(d,0,512); memset(d,' ',11); d[0]='.'; d[11]=0x10; put16(d+20,d0>>16); put16(d+26,d0&0xffffu);
 memset(d+32,' ',11); d[32]='.'; d[33]='.'; d[43]=0x10; put16(d+52,up>>16); put16(d+58,up&0xffffu);
 if((r=wr(lba_of(d0,0),d,0))) return r;
 if((r=link(1))) return r;
 if((r=write_entry(free_slot,name,0x10,d0,0))) return r;
 *dir=d0; return 0;
}
/* "NAME" is /OS/NAME; "GAMES/DIR/NAME" is /GAMES/DIR/NAME; "APPS/DIR/NAME"
   is /OS/APPS/DIR/NAME (an app's folder); "GAMES/NAME.GMP" and
   "POCKET/NAME.GMP" are a packed game (or its further volume, NAME.G01 to
   NAME.G99) in /GAMES or /POCKET, the Pocket Player's folder, and
   "GAMES/LIBRARY.JSL" the library index. Nothing else. */
static bool pack_name(const char *name) {
 const char *dot=0; for(const char *c=name;*c;c++) if(*c=='.') dot=c;
 if(!dot||dot==name||strlen(dot)!=4||dot[1]!='G') return false;
 return (dot[2]=='M'&&dot[3]=='P')||(dot[2]>='0'&&dot[2]<='9'&&dot[3]>='0'&&dot[3]<='9'&&(dot[2]!='0'||dot[3]!='0'));
}
static int split(const char *path,uint8_t dirs[3][11],unsigned *ndirs,uint8_t nm[11]) {
 static const uint8_t os_name[11]={'O','S',' ',' ',' ',' ',' ',' ',' ',' ',' '};
 char part[3][13]; unsigned n=0,k=0;
 for(const char *p=path;;p++) {
  if(*p=='/'||!*p) {if(!k) return fail("PUT NAME",22); part[n++][k]=0; k=0; if(!*p) break; continue;}
  if(k==12||n==3) return fail("PUT NAME",22);
  part[n][k++]=*p;
 }
 if(n==1) {memcpy(dirs[0],os_name,11);*ndirs=1;return fatput_name(part[0],nm);}
 if(n==2&&(!strcmp(part[0],"GAMES")||!strcmp(part[0],"POCKET"))&&(pack_name(part[1])||(part[0][0]=='G'&&!strcmp(part[1],"LIBRARY.JSL")))) {
  int r=fatput_name(part[0],dirs[0]); if(r) return r;
  *ndirs=1; return fatput_name(part[1],nm);
 }
 if(n!=3||(strcmp(part[0],"GAMES")&&strcmp(part[0],"APPS"))) return fail("PUT NAME",22);
 unsigned d=0; if(part[0][0]=='A') memcpy(dirs[d++],os_name,11); /* APPS lives in /OS */
 for(unsigned i=0;i<2;i++,d++) {int r=fatput_name(part[i],dirs[d]); if(r) return r; for(const char *c=part[i];*c;c++) if(*c=='.') return fail("PUT NAME",22);}
 *ndirs=d; return fatput_name(part[2],nm);
}
int fatput_check(const char *path) {uint8_t d[3][11],nm[11];unsigned n;return split(path,d,&n,nm);}
/* ---- Power-cut recovery for USB MV (and the guard RM uses) ----
 * A move to another folder writes its new entry with MV_MARK set in the reserved
 * NT byte (12) and clears it only after the old name is gone. A cut in between
 * leaves two live names on one cluster chain, the new one marked. The first changing
 * command after boot walks the zone (/GAMES, /POCKET, /OS files, /OS/APPS, MV_MAXC
 * deep) once, keeps each marked entry, deletes the other live name with its start
 * cluster (and its long name), repoints a folder's "..", then clears the mark. */
#define MV_MAXC 6
#define MV_MARK 0x40
static uint32_t ecl(const uint8_t *e) {return ((uint32_t)u16(e+20)<<16)|u16(e+26);}
static bool swept; /* the zone has no marked entry left (cleared on boot and by a cut move) */
/* What zone_walk looks for: mode 0 the first marked entry, mode 1 a live entry starting at
   want other than skip. found: where, its folder, the entry and its long-name group. */
static struct {uint8_t mode; uint32_t want; struct slot skip,at,lfn[20]; bool found; uint32_t parent; uint8_t e[32]; unsigned lfn_n;} zq;
static int zone_dir(uint32_t dir,unsigned part,bool os_top) {
 uint32_t c=dir; lfn_run_n=0; watchdog_feed();
 for(uint32_t hop=0;hop<65536/(v.spc*16)+1;hop++) {
  if(!cluster_ok(c)) return fail("PUT DIR CLUSTER",5);
  for(uint32_t i=0;i<v.spc;i++) {
   uint32_t lba=lba_of(c,i); int r=rd(lba,s); if(r) return r;
   for(uint32_t o=0;o<512;o+=32) {
    const uint8_t *e=s+o;
    if(!e[0]) return 0;
    if(e[0]==0xe5) {lfn_run_n=0;continue;}
    if((e[11]&0x3f)==0x0f) {
     if(e[0]&0x40) lfn_run_n=0;
     if(lfn_run_n<20) {lfn_run[lfn_run_n]=(struct slot){lba,o,true};lfn_run_ord[lfn_run_n]=e[0];lfn_run_sum[lfn_run_n]=e[13];lfn_run_n++;}
     continue;
    }
    uint32_t x=ecl(e);
    if(e[0]!='.'&&!(e[11]&0x08)&&(zq.mode?x&&x==zq.want&&!(lba==zq.skip.lba&&o==zq.skip.off):(e[12]&MV_MARK))) {
     zq.found=true; zq.at=(struct slot){lba,o,true}; zq.parent=dir; memcpy(zq.e,e,32);
     lfn_keep(e); memcpy(zq.lfn,lfn_at,sizeof zq.lfn); zq.lfn_n=lfn_n; return 0;
    }
    lfn_run_n=0;
    if(e[0]!='.'&&(e[11]&0x18)==0x10&&part+1<MV_MAXC&&cluster_ok(x)&&(!os_top||!memcmp(e,"APPS       ",11))) {
     if((r=zone_dir(x,part+1,false))||zq.found) return r;
     if((r=rd(lba,s))) return r;
    }
   }
  }
  uint32_t n; int r=fat_get(c,&n); if(r) return r;
  if(n>=0x0ffffff8u) return 0;
  c=n;
 }
 return fail("PUT DIR LOOP",5);
}
static int zone_walk(void) {
 static const char tops[3][12]={"GAMES      ","POCKET     ","OS         "};
 zq.found=false;
 for(unsigned t=0;t<3;t++) {
  struct slot h,f; uint8_t e[32]; int r=scan(v.root,(const uint8_t *)tops[t],&h,&f,e); if(r) return r;
  if(!h.found||(e[11]&0x1e)!=0x10||!cluster_ok(ecl(e))) continue;
  if((r=zone_dir(ecl(e),1,t==2))||zq.found) return r;
 }
 return 0;
}
/* Another live name in the zone starts at first (never 0): its chain must not be freed. */
static int shared_start(uint32_t first,struct slot self,bool *shared) {
 *shared=false; if(!first) return 0;
 zq.mode=1; zq.want=first; zq.skip=self; int r=zone_walk(); *shared=zq.found; return r;
}
static int set_byte(struct slot at,uint32_t i,uint8_t val) {
 int r=rd(at.lba,s); if(r) return r;
 s[at.off+i]=val; return wr(at.lba,s,0);
}
static int mv_recover(void) {
 for(unsigned round=0;!swept&&round<8;round++) {
  zq.mode=0; int r=zone_walk(); if(r) return r;
  if(!zq.found) {swept=true;break;}
  struct slot keep=zq.at; uint32_t parent=zq.parent,first=ecl(zq.e); bool isdir=zq.e[11]&0x10; uint8_t nt=zq.e[12];
  bool dup; if((r=shared_start(first,keep,&dup))) return r;
  if(dup) { /* the cut move's old name: point the folder's ".." here, then drop the old name */
   struct slot old=zq.at,lfn[20]; unsigned ln=zq.lfn_n; memcpy(lfn,zq.lfn,sizeof lfn);
   if(isdir&&cluster_ok(first)&&!rd(lba_of(first,0),s)&&s[32]=='.'&&s[33]=='.') {
    put16(s+52,parent>>16); put16(s+58,parent&0xffffu); if((r=wr(lba_of(first,0),s,0))) return r;
   }
   if((r=lfn_delete(lfn,ln))||(r=set_byte(old,0,0xe5))) return r;
  }
  if((r=set_byte(keep,12,(uint8_t)(nt&~MV_MARK)))) return r;
 }
 return swept?0:fail("MOV RECOVER",5);
}
/* game: NULL for the cache; else a save folder /OS/SAVES/<game> (strict: no read-only files, no unvalidated chains). */
/* A save folder name: 1 to 8 of A-Z 0-9 _ -, no dot. */
static int save_dir_name(const char *game,uint8_t out[11]) {
 for(const char *c=game;*c;c++) if(*c=='.') return fail("PUT NAME",22);
 return fatput_name(game,out);
}
static int put_file(const char *path,const uint8_t *data,uint32_t bytes,bool cache,const char *game) {
 uint8_t nm[11],e[32],dirs[3][11]; unsigned ndirs;
 int r;
 if(cache) {
  if((r=fatput_name(path,nm))) return r;
  memcpy(dirs[0],"OS         ",11);ndirs=2;
  if(game) {memcpy(dirs[1],"SAVES      ",11);if((r=save_dir_name(game,dirs[2])))return r;ndirs=3;}
  else memcpy(dirs[1],"CACHE      ",11);
 } else if((r=split(path,dirs,&ndirs,nm))) return r;
 if(bytes>FATPUT_MAX) return fail("PUT SIZE",27);
 if((r=asset_volume(&v))) return fail("PUT UNMOUNTED",19);
 taken=0; fc_sec=~0u; heal=true;
 if(!cache&&(r=mv_recover())) return r; /* a replaced file's chain must not be shared with a cut move's other name */
 if((r=rd(v.base,s))) return r;
 if(u16(s+48)&&u16(s+48)!=0xffff&&u16(s+48)>=v.fat-v.base)return fail("PUT FSINFO RANGE",5);
 uint32_t fsinfo=v.base+u16(s+48); if(u16(s+48)==0||u16(s+48)==0xffff) fsinfo=0;
 struct slot hit,free_slot; uint32_t dir=v.root;
 for(unsigned i=0;i<ndirs;i++) {
  if(game&&i==2) { /* a new game folder counts against the cap on /OS/SAVES */
   uint8_t de[32]; struct slot dh,df; uint32_t folders=0;
   if((r=scan(dir,dirs[2],&dh,&df,de))) return r;
   if(!dh.found) {if((r=count_entries(dir,&folders))) return r; if(folders>=FATSAVE_MAX_GAMES) return fail("SAVE GAMES FULL",FATSAVE_EQUOTA);}
  }
  if((r=ensure_dir(dir,i==0,dirs[i],&dir))) return r;
 }
 if((r=scan(dir,nm,&hit,&free_slot,e))) return r;
 uint32_t olds=0;
 if(game&&!hit.found) { /* a new save file counts against the cap on this game's folder */
  uint32_t files=0; if((r=count_entries(dir,&files))) return r;
  if(files>=FATSAVE_MAX_FILES) return fail("SAVE FILES FULL",FATSAVE_EQUOTA);
  if((r=scan(dir,nm,&hit,&free_slot,e))) return r; /* count_entries reused the sector buffer */
 }
 if(hit.found) {
  if(e[11]&(cache&&!game?0x18:0x1f)) return fail("PUT NOT FILE",21); /* cache capability owns regular files, including R/O attributes */
  uint32_t first=((uint32_t)u16(e+20)<<16)|u16(e+26);
  if((r=walk(first,u32(e+28),old,sizeof old/4,&olds))) {
   olds=0; /* malformed cache or save: replace it, but never reclaim an unvalidated chain (it leaks for fsck) */
  }
 } else if(!free_slot.found&&(r=grow(&free_slot))) return r;
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
 fatput_fail="none";
 return cache?0:asset_mount();
}
int fatput(const char *path,const uint8_t *data,uint32_t bytes) {
 int r=fatput_check(path);if(r)return r;
 if(bytes>FATPUT_MAX)return -27;
 /* Installer timestamps are fixed. Unpublish BEFORE any content mutation,
    including a same-name/same-size update or an interrupted install. */
 r=fatcache_invalidate();if(r&&r!=-2)return r;
 return put_file(path,data,bytes,false,0);
}
/* Names, never paths: this entry cannot escape /OS/CACHE. */
int fatcache_put(const char *name,const uint8_t *data,uint32_t bytes) {
 if(asset_cache_readers())return -16;
 return put_file(name,data,bytes,true,0);
}
/* Find an existing cache directory, never create it during removal. */
static int cache_dir(uint32_t *dir,const char *game) {
 int r=asset_volume(&v);if(r)return r;
 fc_sec=~0u;heal=true;*dir=v.root;
 uint8_t gm[11];
 if(game&&(r=save_dir_name(game,gm)))return r;
 const uint8_t *names[3]={(const uint8_t *)"OS         ",(const uint8_t *)(game?"SAVES      ":"CACHE      "),gm};
 for(unsigned i=0;i<(game?3u:2u);i++) {
  struct slot hit,free_slot;uint8_t e[32];
  if((r=scan(*dir,names[i],&hit,&free_slot,e)))return r;
  if(!hit.found)return -2;
  if((e[11]&0x1e)!=0x10)return fail("PUT NOT DIR",20);
  *dir=((uint32_t)u16(e+20)<<16)|u16(e+26);
  if(!cluster_ok(*dir))return -5;
 }
 return 0;
}
static int remove_cache(const char *name,bool discard,const char *game) {
 if(asset_cache_readers())return -16;
 uint8_t nm[11],e[32];uint32_t dir,n=0;struct slot hit,free_slot;
 int r=fatput_name(name,nm);if(r)return r;
 if((r=cache_dir(&dir,game)))return r;
 if((r=rd(v.base,s)))return r;
 if(u16(s+48)&&u16(s+48)!=0xffff&&u16(s+48)>=v.fat-v.base)return fail("PUT FSINFO RANGE",5);
 if((r=scan(dir,nm,&hit,&free_slot,e)))return r;
 if(!hit.found)return -2;
 if(e[11]&(discard?0x18:0x1f))return discard?0:-21;
 if(!discard&&(r=walk(((uint32_t)u16(e+20)<<16)|u16(e+26),u32(e+28),old,sizeof old/4,&n))) {if(!game)return r;n=0;} /* a corrupt save can still be removed: unpublished, chain left for fsck */
 /* Unpublish before freeing. A cut can leak, never reuse a live chain. */
 if((r=rd(hit.lba,s)))return r;
 s[hit.off]=0xe5;
 if((r=wr(hit.lba,s,0)))return r;
 /* Invalidation is committed. Bad cache data must not deny installs. */
 if(discard&&(r=walk(((uint32_t)u16(e+20)<<16)|u16(e+26),u32(e+28),old,sizeof old/4,&n)))return 0;
 for(uint32_t i=0;i<n;i++)if((r=fat_set(old[i],0)))return discard?0:r;
 /* Update FSInfo only after unpublication and release. Unknown stays unknown. */
 if((r=rd(v.base,s)))return discard?0:r;
 uint32_t fsinfo=v.base+u16(s+48);
 if(!u16(s+48)||u16(s+48)==0xffff)return 0;
 if((r=rd(fsinfo,s)))return discard?0:r;
 if(u32(s)==0x41615252u&&u32(s+484)==0x61417272u) {
  uint32_t f=u32(s+488);if(f!=~0u) {
   put32(s+488,f<=v.clusters&&n<=v.clusters-f?f+n:~0u);
   if((r=wr(fsinfo,s,fsinfo)))return discard?0:r;
  }
 }
 return 0;
}
int fatcache_remove(const char *name) {return remove_cache(name,false,0);}
int fatcache_invalidate(void) {return remove_cache("GAMES.IDX",true,0);}
int fatcache_rename(const char *from,const char *to,uint8_t *scratch,uint32_t cap) {
 uint8_t a[11],b[11];int r=fatput_name(from,a);if(r)return r;
 if((r=fatput_name(to,b)))return r;
 if(!memcmp(a,b,11))return -22;
 char path[24]="/OS/CACHE/";size_t n=strlen(from);memcpy(path+10,from,n+1);
 /* /OS/CACHE/ is 10 bytes; use no shared clusters between names. */
 int h=asset_open(path,(uint32_t)(10+n),0);if(h<0)return h;
 uint32_t bytes=0;if((r=asset_size(h,&bytes))||bytes>cap) {asset_close(h);return r?r:-27;}
 uint32_t at=0;
 while(at<bytes) {r=asset_read(h,scratch+at,bytes-at);if(r<=0){asset_close(h);return r?r:-5;}at+=(uint32_t)r;}
 asset_close(h);
 if((r=fatcache_put(to,scratch,bytes)))return r;
 /* COW publication to destination, then remove source. On an interrupted
    rename both names may remain, with INDEPENDENT chains. Readers use only
    the destination. No two-entry FAT rename or cross-link crash window. */
 return fatcache_remove(from);
}
/* ABI 1.7: /OS/SAVES/<GAME>/<NAME> only; plain 8.3 names, never paths. Same
   copy-on-write publication as the cache, but strict about what it replaces. */
int fatsave_put(const char *game,const char *name,const uint8_t *data,uint32_t bytes) {
 if(asset_cache_readers())return -16;
 return put_file(name,data,bytes,true,game);
}
int fatsave_remove(const char *game,const char *name) {return remove_cache(name,false,game);}
int fatsave_rename(const char *game,const char *from,const char *to,uint8_t *scratch,uint32_t cap) {
 uint8_t a[11],b[11],g[11];int r=fatput_name(from,a);if(r)return r;
 if((r=fatput_name(to,b)))return r;
 if((r=save_dir_name(game,g)))return r;
 if(!memcmp(a,b,11))return -22;
 char path[40]="/OS/SAVES/";size_t gl=strlen(game),n=strlen(from);
 memcpy(path+10,game,gl);path[10+gl]='/';memcpy(path+11+gl,from,n+1);
 int h=asset_open(path,(uint32_t)(11+gl+n),0);if(h<0)return h;
 uint32_t bytes=0;if((r=asset_size(h,&bytes))||bytes>cap) {asset_close(h);return r?r:-27;}
 uint32_t at=0;
 while(at<bytes) {r=asset_read(h,scratch+at,bytes-at);if(r<=0){asset_close(h);return r?r:-5;}at+=(uint32_t)r;}
 asset_close(h);
 if((r=fatsave_put(game,to,scratch,bytes)))return r;
 return fatsave_remove(game,from);
}
/* ---- USB MV, RM, RMDIR and LS (move, delete, list; 8.3 names only) ----
 * Paths are real card paths without a leading slash, upper-case 8.3 parts, at
 * most MV_MAXC deep. Changing commands reach only /GAMES and /POCKET (any depth),
 * /OS/APPS (any depth) and plain files in /OS: never the card root, the boot, firmware,
 * slot or control files, /OS/CACHE or /OS/SAVES, and never the top folders
 * themselves. Power-cut order: a rename inside one folder is a single 32-byte
 * directory entry write (a sector write); a move to another folder writes the
 * new entry first (marked, see mv_recover), repoints a moved folder's "..",
 * deletes the old name and clears the mark last; a delete marks the entry
 * deleted first and frees the chain after (unless another live name shares
 * it), so a cut leaks clusters at worst. */
struct mpath {char p[MV_MAXC][13]; uint8_t c[MV_MAXC][11]; unsigned n;};
static int mv_fail(const char *w,int e) {return fail(w,e);}
static int mpath_parse(const char *path,struct mpath *m) {
 unsigned k=0; m->n=0;
 if(!strcmp(path,"/")) return 0; /* the root (LS only) */
 for(const char *p=path;;p++) {
  if(*p=='/'||!*p) {
   if(!k||m->n==MV_MAXC) return mv_fail("MOV PATH",22);
   m->p[m->n][k]=0; if(fatput_name(m->p[m->n],m->c[m->n])) return mv_fail("MOV PATH",22);
   m->n++;k=0; if(!*p) break; continue;
  }
  if(k==12) return mv_fail("MOV PATH",22);
  m->p[m->n][k++]=*p;
 }
 return 0;
}
/* 0 if a changing command may touch this item; *file_only: only a plain file. */
static int mv_zone(const struct mpath *m,bool *file_only) {
 *file_only=false;
 if(m->n>=2&&(!strcmp(m->p[0],"GAMES")||!strcmp(m->p[0],"POCKET"))) return 0;
 if(m->n==2&&!strcmp(m->p[0],"OS")) {*file_only=true;return 0;}
 if(m->n>=3&&!strcmp(m->p[0],"OS")&&!strcmp(m->p[1],"APPS")) return 0;
 return mv_fail("MOV ZONE",13);
}
/* The directory reached through the first count parts of m. */
static int mv_dirs(const struct mpath *m,unsigned count,uint32_t *dir) {
 *dir=v.root;
 for(unsigned i=0;i<count;i++) {
  struct slot hit,fs; uint8_t e[32]; int r=scan(*dir,m->c[i],&hit,&fs,e); if(r) return r;
  if(!hit.found) return mv_fail("MOV MISSING",2);
  if((e[11]&0x1e)!=0x10) return mv_fail("MOV NOT DIR",20);
  *dir=ecl(e); if(!cluster_ok(*dir)) return fail("PUT DIR CLUSTER",5);
 }
 return 0;
}
static int mv_start(void) {
 int r=fatcache_invalidate(); if(r&&r!=-2) return r; /* the games index is stale after any change */
 if(asset_volume(&v)) return fail("PUT UNMOUNTED",19);
 fc_sec=~0u; heal=true; return mv_recover();
}
static int fsinfo_free(uint32_t n) {
 int r=rd(v.base,s); if(r||!n) return r;
 uint32_t off=u16(s+48); if(!off||off==0xffff||off>=v.fat-v.base) return 0;
 uint32_t fsinfo=v.base+off; if((r=rd(fsinfo,s))) return r;
 if(u32(s)==0x41615252u&&u32(s+484)==0x61417272u) {
  uint32_t f=u32(s+488);
  if(f!=~0u) {put32(s+488,f<=v.clusters&&n<=v.clusters-f?f+n:~0u);return wr(fsinfo,s,fsinfo);}
 }
 return 0;
}
/* n clusters were taken: lower FSInfo's free count (an unknown count stays unknown). */
static int fsinfo_used(uint32_t n) {
 int r=rd(v.base,s); if(r) return r;
 uint32_t off=u16(s+48); if(!off||off==0xffff||off>=v.fat-v.base) return 0;
 uint32_t fsinfo=v.base+off; if((r=rd(fsinfo,s))) return r;
 if(u32(s)==0x41615252u&&u32(s+484)==0x61417272u) {
  uint32_t f=u32(s+488);
  if(f!=~0u) {put32(s+488,f<=v.clusters&&f>=n?f-n:~0u);return wr(fsinfo,s,fsinfo);}
 }
 return 0;
}
int fatfs_mv(const char *from,const char *to) {
 struct mpath a,b; bool fo_a,fo_b; int r;
 if((r=mpath_parse(from,&a))||(r=mpath_parse(to,&b))) return r;
 if(!a.n||!b.n) return mv_fail("MOV PATH",22);
 if((r=mv_zone(&a,&fo_a))||(r=mv_zone(&b,&fo_b))) return r;
 if(b.n>=a.n) {bool same=true; for(unsigned i=0;i<a.n;i++) if(memcmp(a.c[i],b.c[i],11)) same=false; if(same) return mv_fail("MOV SELF",22);}
 if((r=mv_start())) return r;
 struct slot hit,fs,h2,f2; uint8_t e[32],e2[32]; uint32_t pa,pb;
 if((r=mv_dirs(&a,a.n-1,&pa))||(r=scan(pa,a.c[a.n-1],&hit,&fs,e))) return r;
 if(!hit.found) return mv_fail("MOV MISSING",2);
 struct slot own[20]; unsigned own_n=lfn_n; memcpy(own,lfn_at,sizeof own);
 bool isdir=e[11]&0x10;
 if(isdir?(e[11]&0x1e)!=0x10:(e[11]&0x1f)) return mv_fail(isdir?"MOV NOT DIR":"MOV NOT FILE",isdir?20:21);
 if(isdir&&(fo_a||fo_b)) return mv_fail("MOV NOT FILE",21);
 if((r=mv_dirs(&b,b.n-1,&pb))||(r=scan(pb,b.c[b.n-1],&h2,&f2,e2))) return r;
 if(h2.found) return mv_fail("MOV EXISTS",17);
 memcpy(e2,e,32); memcpy(e2,b.c[b.n-1],11);
 if(pa==pb) { /* one entry, one sector write */
  if((r=lfn_delete(own,own_n))) return r; /* the old long name goes first: a cut leaves a plain short name */
  if((r=rd(hit.lba,s))) return r;
  memcpy(s+hit.off,b.c[b.n-1],11); if((r=wr(hit.lba,s,0))) return r;
  fatput_fail="none"; return asset_mount();
 }
 uint32_t first=ecl(e);
 if(isdir) { /* check the moved folder's ".." before any write */
  if(!cluster_ok(first)||(r=rd(lba_of(first,0),s))) return r?r:fail("PUT DIR CLUSTER",5);
  if(s[32]!='.'||s[33]!='.') return fail("PUT DIR CLUSTER",5);
 }
 if(!f2.found) {if((r=grow(&f2))||(r=fsinfo_used(1))) return r;}
 swept=false; e2[12]|=MV_MARK; /* the new name is marked until the old one is gone (mv_recover) */
 if((r=rd(f2.lba,s))) return r;
 memcpy(s+f2.off,e2,32); if((r=wr(f2.lba,s,0))) return r;
 if(isdir) {
  if((r=rd(lba_of(first,0),s))) return r;
  put16(s+52,pb>>16); put16(s+58,pb&0xffffu); if((r=wr(lba_of(first,0),s,0))) return r;
 }
 if((r=lfn_delete(own,own_n))||(r=set_byte(hit,0,0xe5))) return r; /* long name first: a cut leaves a plain short name */
 if((r=set_byte(f2,12,(uint8_t)(e2[12]&~MV_MARK)))) return r;
 swept=true; fatput_fail="none"; return asset_mount();
}
/* The chain from first is sound before it is freed: every cluster in range (not free, bad or
   reserved), the same in both FATs, no cluster twice, and a file's length matching its size. */
static int chain_check(uint32_t first,uint32_t bytes,bool dir) {
 uint32_t need=dir?0:(bytes+v.spc*512-1)/(v.spc*512),c=first,n=0;
 if(!dir&&!need) return first?fail("PUT OLD CHAIN",5):0;
 while(true) {
  if(!cluster_ok(c)||n>v.clusters||(!dir&&n>=need)) return fail("PUT OLD CHAIN",5);
  n++; uint32_t nx; int r=fat_get(c,&nx); if(r) return r;
  if(!(n&0xff)) watchdog_feed();
  if(nx>=0x0ffffff8u) return dir||n==need?0:fail("PUT OLD CHAIN",5);
  c=nx;
 }
}
static int fatfs_remove(const char *path,bool want_dir) {
 struct mpath m; bool fo; int r;
 if((r=mpath_parse(path,&m))) return r;
 if(!m.n) return mv_fail("MOV PATH",22);
 if((r=mv_zone(&m,&fo))) return r;
 if(want_dir&&fo) return mv_fail("MOV NOT DIR",20);
 if((r=mv_start())) return r;
 struct slot hit,fs; uint8_t e[32]; uint32_t pd;
 if((r=mv_dirs(&m,m.n-1,&pd))||(r=scan(pd,m.c[m.n-1],&hit,&fs,e))) return r;
 if(!hit.found) return mv_fail("MOV MISSING",2);
 if(want_dir?(e[11]&0x1e)!=0x10:(e[11]&0x1f)) return mv_fail(want_dir?"MOV NOT DIR":"MOV NOT FILE",want_dir?20:21);
 uint32_t first=ecl(e);
 struct slot own[20]; unsigned own_n=lfn_n; memcpy(own,lfn_at,sizeof own);
 if(want_dir) {
  if(!cluster_ok(first)) return fail("PUT DIR CLUSTER",5);
  uint32_t n; if((r=count_entries(first,&n))) return r;
  if(n) return mv_fail("MOV NOT EMPTY",39);
 }
 if((r=chain_check(first,want_dir?0:u32(e+28),want_dir))) return r; /* a damaged chain is never freed */
 bool shared; if((r=shared_start(first,hit,&shared))) return r; /* another live name keeps it: take the name only */
 if((r=lfn_delete(own,own_n))) return r;
 if((r=rd(hit.lba,s))) return r;
 s[hit.off]=0xe5; if((r=wr(hit.lba,s,0))) return r; /* unpublished: now the chain may be freed */
 uint32_t freed=0,c=first;
 while(!shared&&cluster_ok(c)&&freed<=v.clusters) {
  uint32_t nx; if((r=fat_get(c,&nx))||(r=fat_set(c,0))) return r;
  freed++; if(!(freed&0xff)) watchdog_feed();
  if(nx>=0x0ffffff8u||!nx) break;
  c=nx;
 }
 if((r=fsinfo_free(freed))) return r;
 fatput_fail="none"; return asset_mount();
}
int fatfs_rm(const char *path) {return fatfs_remove(path,false);}
int fatfs_rmdir(const char *path) {return fatfs_remove(path,true);}
/* Up to cap-1 bytes of "ENT <D|F> <size> <attr hex> <NAME>\n" lines for a folder, from raw
   slot index `from`; *next is where to resume (~0u at the end). Names a long-name entry
   starts with a dot (macOS ._*, .Spotlight-V100, .fseventsd, .Trashes) are skipped and
   counted in *skipped. Returns the lines written or a negative errno. */
int fatfs_ls(const char *path,uint32_t from,char *out,uint32_t cap,uint32_t *next,uint32_t *skipped) {
 struct mpath m; int r; uint32_t dir,idx=0,grp=0,len=0,lines=0; bool in_lfn=false,dot=false;
 *next=~0u;*skipped=0; out[0]=0;
 if((r=mpath_parse(path,&m))) return r;
 if(asset_volume(&v)) return fail("PUT UNMOUNTED",19);
 fc_sec=~0u; heal=false; /* listing never writes */
 if((r=mv_dirs(&m,m.n,&dir))) return r;
 uint32_t c=dir;
 for(uint32_t hop=0;hop<65536/(v.spc*16)+1;hop++) {
  if(!cluster_ok(c)) return fail("PUT DIR CLUSTER",5);
  for(uint32_t i=0;i<v.spc;i++) {
   if((r=rd(lba_of(c,i),s))) return r;
   for(uint32_t o=0;o<512;o+=32,idx++) {
    const uint8_t *e=s+o;
    if(!e[0]) return 0;
    if(e[0]==0xe5) {in_lfn=false;continue;}
    if(!in_lfn) {grp=idx;dot=false;}
    if((e[11]&0x3f)==0x0f) {in_lfn=true;if((e[0]&0x1f)==1) dot=e[1]=='.'&&!e[2];continue;}
    in_lfn=false;
    if((e[11]&0x08)||e[0]=='.'||grp<from) continue;
    if(dot) {(*skipped)++;continue;}
    char name[13];unsigned k=0;
    for(unsigned j=0;j<8&&e[j]!=' ';j++) name[k++]=(char)(e[j]==5?0xe5:e[j]);
    if(e[8]!=' ') {name[k++]='.';for(unsigned j=8;j<11&&e[j]!=' ';j++) name[k++]=(char)e[j];}
    name[k]=0;
    char line[48];unsigned q=0; uint32_t sz=(e[11]&0x10)?0:u32(e+28);
    memcpy(line,(e[11]&0x10)?"ENT D ":"ENT F ",6);q=6;
    char dg[11];unsigned nd=0; do {dg[nd++]=(char)('0'+sz%10);sz/=10;} while(sz);
    while(nd) line[q++]=dg[--nd];
    line[q++]=' ';line[q++]="0123456789ABCDEF"[e[11]>>4];line[q++]="0123456789ABCDEF"[e[11]&15];line[q++]=' ';
    memcpy(line+q,name,k);q+=k;line[q++]='\n';
    if(len+q>=cap) {*next=grp;return (int)lines;}
    memcpy(out+len,line,q);len+=q;out[len]=0;lines++;
   }
  }
  uint32_t n; if((r=fat_get(c,&n))) return r;
  if(n>=0x0ffffff8u) return (int)lines;
  c=n;
 }
 return fail("PUT DIR LOOP",5);
}
