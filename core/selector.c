/* SPDX-License-Identifier: MIT */
#include "kernel.h"
void service_io(void) {} /* selector deliberately has UART/card-reader rescue */
void selector_main(void) {
 uart_init(); uart_puts("sandbox selector M0\r\n");
 uint32_t ram[2]={0,0};
 if(!property(0x10005,ram,2)||ram[0]||ram[1]<128*1024*1024u) { uart_puts("Unsupported ARM RAM; card-reader rescue\r\n"); for(;;){} }
 if(!store_init()) { uart_puts("SD/FAT invalid at ");uart_puts(store_fail);uart_puts("; card-reader rescue\r\n"); for(;;){} }
 uint32_t slot=0,trial=0,generation=0,bytes;
 uint8_t *target=(uint8_t *)(RUNTIME_BASE-256);
 if(control[0]==CONTROL_MAGIC) {
  slot=control[3]; generation=control[6];
  if(control[4]!=NO_SLOT&&!control[5]) {
   if(slot_load(control[4],target,&bytes)) {
    uint32_t next[128]; memcpy(next,control,512); next[5]=1;
    if(control_commit(next)) {slot=control[4];trial=1;generation=control[7];}
   }
  }
  if(!trial&&!slot_load(slot,target,&bytes)) {uart_puts("Confirmed slot invalid; card-reader rescue\r\n");for(;;){}}
 } else {
  if(!slot_load(0,target,&bytes)) {slot=1;if(!slot_load(1,target,&bytes)) {uart_puts("Both slots invalid; card-reader rescue\r\n");for(;;){}}}
  uart_puts("Metadata lost: unconfirmed recovery, updates locked\r\n");
 }
 {uint32_t sdb[8]; sd_export(sdb); memcpy((void *)0x7040,sdb,32);} /* card state for the runtime */
 volatile uint32_t *h=(volatile uint32_t *)0x7000;
 h[0]=HANDOFF_MAGIC;h[1]=1;h[2]=slot;h[3]=trial;h[4]=generation;h[5]=REG(PERIPH+0x100020);h[6]=ram[1];h[7]=crc32((const void *)h,28);
 uart_puts(trial?"Launch trial\r\n":"Launch confirmed/recovery\r\n");
 watchdog_feed(); barrier();
 uint32_t z=0; __asm__ volatile("mcr p15,0,%0,c7,c5,0\n mcr p15,0,%0,c7,c10,4\n mcr p15,0,%0,c7,c5,4"::"r"(z):"memory");
 ((void (*)(void))RUNTIME_BASE)(); for(;;){}
}
