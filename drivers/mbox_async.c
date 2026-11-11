/* SPDX-License-Identifier: MIT; original implementation from BCM2835 manual. */
/* Asynchronous mailbox requests (power). Split out so a host test can fake the mailbox. */
#include "kernel.h"
/* Runtime power requests use a second strongly ordered mailbox buffer. All
 * readers route its reply, including synchronous framebuffer/SD/CEC properties.
 * No IRQ-side mailbox work and no user pointer is retained. */
/* Two buffers alternate per request, so a reply that arrives after its request
 * was abandoned (lost-reply timeout) is told apart from the next request's. */
#define ASYNC_ABANDON_US 1000000u
#ifdef HOST_TEST
extern volatile uint32_t *const async_buffers[2];extern volatile uint32_t *const test_mbox;
#define ASYNC_ADDR(i) (0xc0080108u+(i)*0x40u)
#define MBOX(o) test_mbox[((o)-0xb880)/4]
#define DRAIN_WB() ((void)0)
#else
static volatile uint32_t *const async_buffers[2]={(volatile uint32_t *)0x80100,(volatile uint32_t *)0x80140};
#define ASYNC_ADDR(i) (0xc0080108u+(i)*0x40u)
#define MBOX(o) REG(PERIPH+(o))
#define DRAIN_WB() do{uint32_t z_=0;__asm__ volatile("mcr p15,0,%0,c7,c10,4"::"r"(z_):"memory");}while(0)
#endif
static bool async_owned,async_ready,async_late;
static unsigned async_which;
static uint32_t async_since;
#define async_buffer async_buffers[async_which]
void mailbox_route(uint32_t reply) {
 if(async_owned&&reply==ASYNC_ADDR(async_which)) async_ready=true;
}
bool property_async_start(uint32_t tag,const uint32_t *values,unsigned words) {
 if(async_owned||!words||words>3||MBOX(0xb898)&0x80000000u) return false;
 async_which^=1;
 async_buffer[0]=(words+6)*4;async_buffer[1]=0;async_buffer[2]=tag;
 async_buffer[3]=words*4;async_buffer[4]=0;
 for(unsigned i=0;i<words;i++) async_buffer[5+i]=values[i];
 async_buffer[5+words]=0;
 DRAIN_WB();
 async_owned=true;async_ready=async_late=false;async_since=micros();
 MBOX(0xb8a0)=ASYNC_ADDR(async_which);barrier();return true;
}
int property_async_poll(uint32_t *values,unsigned words) {
 if(!async_owned||!words||words>3) return -5;
 /* Bounded reads: never spin if a mailbox source is continuously active. */
 for(unsigned i=0;i<16&&!(MBOX(0xb898)&0x40000000u);i++)
  mailbox_route(MBOX(0xb880));
 barrier();
 if(async_ready) {
  async_owned=false;
  if(async_buffer[1]!=0x80000000u||!(async_buffer[4]&0x80000000u)||
     (async_buffer[4]&0x7fffffffu)<words*4) return -5;
  for(unsigned i=0;i<words;i++) values[i]=async_buffer[5+i];
  return 1;
 }
 /* A reply that never comes: give up ownership so later requests can run.
  * A late reply to the abandoned buffer is ignored by address in mailbox_route. */
 if(expired(async_since,ASYNC_ABANDON_US)) {async_owned=false;return -5;}
 if(!async_late&&expired(async_since,100000)) {async_late=true;return -110;}
 return 0;
}
