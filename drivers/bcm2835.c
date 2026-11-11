/* SPDX-License-Identifier: MIT; original implementation from BCM2835 manual. */
#include "kernel.h"
#include "hdmi_audio.h"
#define GPIO (PERIPH+0x200000)
void gpio_mode(unsigned p,unsigned m) { uint32_t a=GPIO+(p/10)*4, s=(p%10)*3; REG(a)=(REG(a)&~(7u<<s))|(m<<s); barrier(); }
void gpio_set(unsigned p,bool v) { REG(GPIO+(v?0x1c:0x28)+(p/32)*4)=1u<<(p%32); barrier(); }
bool gpio_get(unsigned p) { return (REG(GPIO+0x34+(p/32)*4)>>(p%32))&1; }
void delay_us(uint32_t us) { uint32_t s=micros(); while(!expired(s,us)) service_io(); }
static uint32_t table[4096] __attribute__((aligned(16384)));
const uint32_t *mmu_table(void) { return table; }
/* Noncached mailbox page lies outside cacheable sections (see mmu_init). */
const char *property_fail="none";
void mailbox_route(uint32_t reply);
/* The message page (0x80000) is strongly ordered, so no cache maintenance is
 * needed. The GPU gets the 0xC0000000 alias (uncached, bypasses its L2), so a
 * response can never sit in a GPU cache line the ARM does not see. */
/* Post a whole tag message (size word first, at most 64 words) and copy the
 * reply back. Bounded by `timeout` microseconds; never touches the reply on
 * failure. Used by single-tag calls and by multi-tag framebuffer allocation. */
bool property_message(uint32_t *m,unsigned words,uint32_t timeout) {
 volatile uint32_t *b=(volatile uint32_t *)0x80000;
 if(words<3||words>64||m[0]!=words*4) {property_fail="MBOX ARGS";return false;}
 for(unsigned i=0;i<words;i++) b[i]=m[i];
 uint32_t z=0; __asm__ volatile("mcr p15,0,%0,c7,c10,4":: "r"(z):"memory"); /* drain write buffer */
 uint32_t s=micros();
 while(REG(PERIPH+0xb898)&0x80000000u) if(expired(s,100000)) {property_fail="MBOX FULL";return false;}
 while(!(REG(PERIPH+0xb898)&0x40000000u)) mailbox_route(REG(PERIPH+0xb880)); /* drop stale replies */
 REG(PERIPH+0xb8a0)=0xc0080008u; barrier();
 while(!expired(s,timeout)) {
  if(!(REG(PERIPH+0xb898)&0x40000000u)) {
   uint32_t r=REG(PERIPH+0xb880); barrier();
   if(r!=0xc0080008u) {mailbox_route(r);continue;} /* some other reply; keep waiting */
   if(b[1]!=0x80000000u) {property_fail="MBOX RESP";return false;}
   for(unsigned i=0;i<words;i++) m[i]=b[i];
   return true;
  }
 }
 property_fail="MBOX TIMEOUT"; return false;
}
static bool property_once(uint32_t tag,uint32_t *v,unsigned n,uint32_t timeout) {
 uint32_t m[14]; m[0]=(n+6)*4; m[1]=0; m[2]=tag; m[3]=n*4; m[4]=n*4;
 for(unsigned i=0;i<n;i++) { m[5+i]=v[i]; }
 m[5+n]=0;
 if(!property_message(m,n+6,timeout)) return false;
 if(!(m[4]&0x80000000u)) {property_fail="MBOX TAG";return false;}
 for(unsigned i=0;i<n;i++) { v[i]=m[5+i]; }
 return true;
}
bool property(uint32_t tag,uint32_t *v,unsigned n) {
 if(n>8) {property_fail="MBOX ARGS";return false;}
 uint32_t copy[8]; for(unsigned i=0;i<n;i++) copy[i]=v[i];
 for(unsigned attempt=0;attempt<3;attempt++) {
  for(unsigned i=0;i<n;i++) v[i]=copy[i];
  if(property_once(tag,v,n,tag==0x28001?1000000:100000)) return true; /* power waits for stable */
 }
 return false;
}
void uart_init(void) {
 uint32_t v[3]={2,3000000,0}; property(0x38002,v,3);
 REG(PERIPH+0x201030)=0;
 gpio_mode(14,4); gpio_mode(15,4);
 REG(GPIO+0x94)=0; delay_us(5); REG(GPIO+0x98)=(1<<14)|(1<<15); delay_us(5); REG(GPIO+0x98)=0;
 REG(PERIPH+0x201044)=0x7ff;
 REG(PERIPH+0x201024)=1; REG(PERIPH+0x201028)=40;
 REG(PERIPH+0x20102c)=0x70; REG(PERIPH+0x201030)=0x301;
}
void uart_puts(const char *s) {
 while(*s) { uint32_t t=micros(); while(REG(PERIPH+0x201018)&0x20) if(expired(t,2000)) return; REG(PERIPH+0x201000)=(uint8_t)*s++; }
}
volatile uint32_t ticks;
void irq_init(void) {
 REG(PERIPH+0xb21c)=~0u; REG(PERIPH+0xb220)=~0u; REG(PERIPH+0xb224)=~0u;
 REG(PERIPH+0x3000)=2; REG(PERIPH+0x3010)=micros()+10000;
 REG(PERIPH+0x3000)=8;
 REG(PERIPH+0xb210)=2|8;
 __asm__ volatile("cpsie i":::"memory");
}
/* Timer compare 3 is a one-shot idle wake. Compare 1 remains the 10 ms
 * scheduler tick, preserving user slices and heartbeat behavior. ARM1176's
 * CP15 WFI wakes for a pending IRQ even with CPSR.I set. Mask across arming
 * and WFI so an IRQ cannot be consumed between the check and wait. */
void idle_wait(uint32_t us) {
 if(!us) return;
 uint32_t flags,z=0;
 __asm__ volatile("mrs %0,cpsr\n cpsid i":"=r"(flags)::"memory","cc");
 REG(PERIPH+0x3000)=8;
 uint32_t compare=micros()+us;
 REG(PERIPH+0x3018)=compare;
 barrier();
 if((int32_t)(compare-micros())>0)
 __asm__ volatile("mcr p15,0,%0,c7,c10,4\n mcr p15,0,%0,c7,c0,4"::"r"(z):"memory");
 __asm__ volatile("msr cpsr_c,%0"::"r"(flags):"memory","cc");
}
void irq_handler(void) {
 if(REG(PERIPH+0x3000)&8) REG(PERIPH+0x3000)=8;
 if(REG(PERIPH+0x3000)&2) { REG(PERIPH+0x3000)=2; REG(PERIPH+0x3010)=micros()+10000; ticks++; }
#ifndef SELECTOR
 if(REG(PERIPH+0xb204)&0x007e0000u) { hdmi_audio_irq(); }
 if(REG(PERIPH+0xb204)&(1<<9)) { usb_irq(); }
#endif
}
void watchdog_feed(void) {
 REG(PERIPH+0x100024)=0x5a080000; /* 8 seconds, 16.16 clock */
 REG(PERIPH+0x10001c)=0x5a000020|(REG(PERIPH+0x10001c)&~0x30);
}
void reboot(void) { REG(PERIPH+0x100024)=0x5a000010; REG(PERIPH+0x10001c)=0x5a000020; for(;;) {} }
void panic(unsigned kind) { char n[12]; number(n,kind); uart_puts("PANIC exception "); uart_puts(n); uart_puts("\r\n"); for(;;) {} }
/* Map [phys, phys+bytes) as privileged normal non-cacheable, execute-never
 * sections (the HDMI framebuffer). Only above the 128 MiB kernel map and
 * below 512 MiB; existing entries are never replaced. */
bool mmu_map_uncached(uint32_t phys,uint32_t bytes) {
 if(!bytes||phys<0x10000000u||phys>=0x20000000u||bytes>0x20000000u-phys) return false; /* below 256 MiB is the kernel map and the user image window */
 for(uint32_t i=phys>>20;i<=(phys+bytes-1)>>20;i++) if(table[i]&&table[i]!=((i<<20)|0x1412u)) return false;
 for(uint32_t i=phys>>20;i<=(phys+bytes-1)>>20;i++) table[i]=(i<<20)|0x1412u; /* TEX=001 C=B=0, AP=01, XN */
 uint32_t z=0;
 __asm__ volatile("mcr p15,0,%0,c7,c10,4\n mcr p15,0,%0,c8,c7,0\n mcr p15,0,%0,c7,c5,4"::"r"(z):"memory");
 return true;
}
void mmu_init(void) {
 /* M0 privileged identity map; no user execution. First section uncached for
    vectors, selector, mailbox. Runtime RAM write-through, MMIO device+XN. */
 for(unsigned i=0;i<4096;i++) table[i]=0;
 for(unsigned i=0;i<128;i++) table[i]=(i<<20)|0x402|(i?8:0); /* AP=01, client domain */
 for(unsigned i=0x200;i<0x210;i++) table[i]=(i<<20)|0x416; /* device, XN */
 uint32_t z=0, t=(uint32_t)table, d=1,c;
 __asm__ volatile("mcr p15,0,%0,c2,c0,2\n mcr p15,0,%1,c2,c0,0\n mcr p15,0,%2,c3,c0,0\n mcr p15,0,%0,c8,c7,0\n mcr p15,0,%0,c7,c10,4"::"r"(z),"r"(t),"r"(d):"memory");
 __asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(c)); c|=0x1005|(1u<<23); /* XP extended tables */
 __asm__ volatile("mcr p15,0,%0,c1,c0,0\n mcr p15,0,%1,c7,c5,4"::"r"(c),"r"(z):"memory");
}
/* The firmware starts the ARM below its rated clock and there is no governor
   here: ask for the maximum (clock id 3). Branch prediction (SCTLR.Z) is safe
   because user_enter already flushes the predictor when the tables change. */
void cpu_speed(void) {
 uint32_t was[2]={3,0},max[2]={3,0},set[3]={3,0,0};
 property(0x30002,was,2);property(0x30004,max,2);
 if(max[1]) {set[1]=max[1];property(0x38002,set,3);}
 uint32_t c; __asm__ volatile("mrc p15,0,%0,c1,c0,0":"=r"(c)); c|=1u<<11; __asm__ volatile("mcr p15,0,%0,c1,c0,0"::"r"(c):"memory");
 uint32_t now[2]={3,0};property(0x30002,now,2);
 char line[80],*p=line,t[12];
 const char *a="CPU arm clock ";while(*a)*p++=*a++;
 number(t,was[1]/1000000u);for(char *q=t;*q;)*p++=*q++;
 a=" MHz, now ";while(*a)*p++=*a++;
 number(t,now[1]/1000000u);for(char *q=t;*q;)*p++=*q++;
 a=" MHz (max ";while(*a)*p++=*a++;
 number(t,max[1]/1000000u);for(char *q=t;*q;)*p++=*q++;
 a=" MHz), branch prediction on";while(*a)*p++=*a++;*p=0;
 boot_note(line);
}
