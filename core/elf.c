/* SPDX-License-Identifier: MIT; original ELF32 validator and loader, written
 * from the ELF gABI and the Arm ELF supplement (aaelf32). All arithmetic is
 * done in 64 bits before any comparison, so no field can wrap a check. */
#include "kernel.h"
#include "elf.h"
#include "uspace.h"
static uint16_t h16(const uint8_t *p) {return (uint16_t)(p[0]|(p[1]<<8));}
static uint32_t w32(const uint8_t *p) {return (uint32_t)p[0]|((uint32_t)p[1]<<8)|((uint32_t)p[2]<<16)|((uint32_t)p[3]<<24);}
#define PT_LOAD 1u
#define PT_DYNAMIC 2u
#define PT_INTERP 3u
#define PT_NOTE 4u
#define PT_SHLIB 5u
#define PT_PHDR 6u
#define PT_TLS 7u
#define PT_GNU_STACK 0x6474e551u
#define PT_ARM_EXIDX 0x70000001u
#define PT_ARM_ATTRIBUTES 0x70000003u
const char *elf_check(const uint8_t *f,uint32_t n,struct elf_plan *p) {
 memset(p,0,sizeof(*p));
 if(!f||n<52) return "ELF SHORT";
 if(n>ELF_MAX_FILE) return "ELF TOO BIG";
 if(f[0]!=0x7f||f[1]!='E'||f[2]!='L'||f[3]!='F') return "ELF MAGIC";
 if(f[4]!=1) return f[4]==2?"ELF64":"ELF CLASS";
 if(f[5]!=1) return "ELF ENDIAN";
 if(f[6]!=1||w32(f+20)!=1) return "ELF VERSION";
 if(f[7]!=0&&f[7]!=97) return "ELF OSABI"; /* none, or ARM's historic value */
 uint16_t type=h16(f+16);
 if(type!=2) return type==3?"ELF PIE":"ELF TYPE";
 if(h16(f+18)!=40) return "ELF MACHINE";
 uint32_t entry=w32(f+24),phoff=w32(f+28),shoff=w32(f+32),flags=w32(f+36);
 if((flags>>24)!=5) return "ELF EABI";
 if(flags&0x400u) return "ELF HARD FLOAT";
 if(flags&0x00fffbffu&~0x200u) return "ELF FLAGS"; /* only soft-float (0x200) may be set */
 if(h16(f+40)!=52||h16(f+42)!=32) return "ELF HEADER SIZE";
 uint32_t phnum=h16(f+44),shentsize=h16(f+46),shnum=h16(f+48);
 if(!phnum||phnum>ELF_MAX_PHDRS) return "ELF PHNUM";
 if(phoff<52||(uint64_t)phoff+phnum*32ull>n||phoff%4) return "ELF PHDR RANGE";
 if(entry&1u) return "ELF THUMB";
 if(entry&3u) return "ELF ENTRY ALIGN";
 uint64_t pages=0;
 for(uint32_t i=0;i<phnum;i++) {
  const uint8_t *ph=f+phoff+i*32;
  uint32_t t=w32(ph),off=w32(ph+4),va=w32(ph+8),fs=w32(ph+16),ms=w32(ph+20),fl=w32(ph+24),al=w32(ph+28);
  switch(t) {
   case 0: case PT_NOTE: case PT_PHDR: case PT_ARM_EXIDX: case PT_ARM_ATTRIBUTES: continue;
   case PT_GNU_STACK: if(fl&1u) return "ELF EXEC STACK"; continue;
   case PT_INTERP: return "ELF INTERP";
   case PT_DYNAMIC: return "ELF DYNAMIC"; /* runtime relocations, imports */
   case PT_SHLIB: return "ELF SHLIB";
   case PT_TLS: return "ELF TLS";
   case PT_LOAD: break;
   default: return "ELF PHDR TYPE";
  }
  if(!ms) continue;
  if(fl&~7u) return "ELF SEG FLAGS";
  if((fl&2u)&&(fl&1u)) return "ELF WX";
  if(!(fl&7u)) return "ELF SEG FLAGS";
  if(fs>ms) return "ELF FILESZ";
  if((uint64_t)off+fs>n) return "ELF SEG FILE";
  if(al>1&&((al&(al-1))||(va-off)%al)) return "ELF ALIGN";
  uint64_t start=va&~0xfffu,end=((uint64_t)va+ms+0xfffu)&~0xfffull;
  if(va<U_IMAGE_BASE||(uint64_t)va+ms>U_IMAGE_END) return "ELF SEG RANGE";
  if(p->count==ELF_MAX_SEGMENTS) return "ELF SEGMENTS";
  for(uint32_t j=0;j<p->count;j++) { /* whole pages: no page shared by two segments */
   uint64_t s2=p->seg[j].va&~0xfffu,e2=((uint64_t)p->seg[j].va+p->seg[j].memsz+0xfffu)&~0xfffull;
   if(start<e2&&s2<end) return "ELF OVERLAP";
  }
  pages+=(end-start)/4096;
  if(pages*4096>U_IMAGE_QUOTA) return "ELF QUOTA";
  p->seg[p->count++]=(struct elf_seg){va,ms,fs,off,((fl&4u)?U_R:0)|((fl&2u)?U_W:0)|((fl&1u)?U_X:0)};
 }
 if(!p->count) return "ELF NO LOAD";
 bool in_text=false;
 for(uint32_t j=0;j<p->count;j++) {const struct elf_seg *s=&p->seg[j]; if((s->prot&U_X)&&entry>=s->va&&(uint64_t)entry+4<=(uint64_t)s->va+s->filesz) in_text=true;}
 if(!in_text) return "ELF ENTRY";
 if(shoff) { /* optional section table: if present it must be sane and hold no runtime relocations */
  if(shentsize!=40||shnum>256||shoff%4||(uint64_t)shoff+shnum*40ull>n) return "ELF SHDR RANGE";
  for(uint32_t i=0;i<shnum;i++) {
   const uint8_t *sh=f+shoff+i*40; uint32_t t=w32(sh+4),fl=w32(sh+8);
   if((t==4||t==9||t==6||t==11||t==5||t==0x6ffffff6u)&&(fl&2u)) return "ELF RELOC"; /* RELA, REL, DYNAMIC, DYNSYM, HASH, GNU_HASH */
  }
 }
 p->entry=entry;p->bytes=(uint32_t)(pages*4096);
 return NULL;
}
int elf_load(const uint8_t *f,const struct elf_plan *p) {
 for(uint32_t j=0;j<p->count;j++) {
  const struct elf_seg *s=&p->seg[j]; uint32_t start=s->va&~0xfffu,end=(s->va+s->memsz+0xfffu)&~0xfffu;
  int r=us_map(start,end-start,s->prot); if(r) return r;
  uint8_t *dst; if(us_span(s->va,s->filesz?s->filesz:1,U_R,&dst)<0) return -14;
  memcpy(dst,f+s->offset,s->filesz); /* the rest of the pages is already zero */
 }
 return 0;
}
