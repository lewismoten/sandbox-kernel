/* SPDX-License-Identifier: MIT; original implementation from BCM2835 manual. */
#include "kernel.h"
#define GPIO (PERIPH+0x200000)
void gpio_mode(unsigned p,unsigned m) { uint32_t a=GPIO+(p/10)*4, s=(p%10)*3; REG(a)=(REG(a)&~(7u<<s))|(m<<s); barrier(); }
void gpio_set(unsigned p,bool v) { REG(GPIO+(v?0x1c:0x28)+(p/32)*4)=1u<<(p%32); barrier(); }
bool gpio_get(unsigned p) { return (REG(GPIO+0x34+(p/32)*4)>>(p%32))&1; }
void delay_us(uint32_t us) { uint32_t s=micros(); while(!expired(s,us)) service_io(); }
/* Noncached mailbox page lies outside cacheable sections (see mmu_init). */
const char *property_fail="none";
/* The message page (0x80000) is strongly ordered, so no cache maintenance is
 * needed. The GPU gets the 0xC0000000 alias (uncached, bypasses its L2), so a
 * response can never sit in a GPU cache line the ARM does not see. */
static bool property_once(uint32_t tag,uint32_t *v,unsigned n) {
 volatile uint32_t *b=(volatile uint32_t *)0x80000;
 b[0]=(n+6)*4; b[1]=0; b[2]=tag; b[3]=n*4; b[4]=n*4;
 for(unsigned i=0;i<n;i++) b[5+i]=v[i]; b[5+n]=0;
 uint32_t z=0; __asm__ volatile("mcr p15,0,%0,c7,c10,4":: "r"(z):"memory"); /* drain write buffer */
 uint32_t s=micros();
 while(REG(PERIPH+0xb898)&0x80000000u) if(expired(s,100000)) {property_fail="MBOX FULL";return false;}
 while(!(REG(PERIPH+0xb898)&0x40000000u)) (void)REG(PERIPH+0xb880); /* drop stale replies */
 REG(PERIPH+0xb8a0)=0xc0080008u; barrier();
 while(!expired(s,100000)) {
  if(!(REG(PERIPH+0xb898)&0x40000000u)) {
   uint32_t r=REG(PERIPH+0xb880); barrier();
   if(r!=0xc0080008u) continue; /* some other reply; keep waiting */
   if(b[1]!=0x80000000u) {property_fail="MBOX RESP";return false;}
   if(!(b[4]&0x80000000u)) {property_fail="MBOX TAG";return false;}
   for(unsigned i=0;i<n;i++) v[i]=b[5+i]; return true;
  }
 }
 property_fail="MBOX TIMEOUT"; return false;
}
bool property(uint32_t tag,uint32_t *v,unsigned n) {
 if(n>8) {property_fail="MBOX ARGS";return false;}
 uint32_t copy[8]; for(unsigned i=0;i<n;i++) copy[i]=v[i];
 for(unsigned attempt=0;attempt<3;attempt++) {
  for(unsigned i=0;i<n;i++) v[i]=copy[i];
  if(property_once(tag,v,n)) return true;
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
 REG(PERIPH+0xb210)=2;
 __asm__ volatile("cpsie i":::"memory");
}
void irq_handler(void) {
 if(REG(PERIPH+0x3000)&2) { REG(PERIPH+0x3000)=2; REG(PERIPH+0x3010)=micros()+10000; ticks++; }
#ifndef SELECTOR
 if(REG(PERIPH+0xb204)&(1<<9)) { extern void usb_irq(void); usb_irq(); }
#endif
}
void watchdog_feed(void) {
 REG(PERIPH+0x100024)=0x5a080000; /* 8 seconds, 16.16 clock */
 REG(PERIPH+0x10001c)=0x5a000020|(REG(PERIPH+0x10001c)&~0x30);
}
void reboot(void) { REG(PERIPH+0x100024)=0x5a000010; REG(PERIPH+0x10001c)=0x5a000020; for(;;) {} }
void panic(unsigned kind) { char n[12]; number(n,kind); uart_puts("PANIC exception "); uart_puts(n); uart_puts("\r\n"); for(;;) {} }
static uint32_t table[4096] __attribute__((aligned(16384)));
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
