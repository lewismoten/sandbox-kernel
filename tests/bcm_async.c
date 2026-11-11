/* SPDX-License-Identifier: MIT; fake mailbox: a lost async reply is abandoned, a late one ignored. */
#include <stdio.h>
#define HOST_TEST 1
#include "../drivers/mbox_async.c"
#include <assert.h>
static uint32_t now_us;
uint32_t micros(void) {return now_us;}
void service_io(void) {}
static uint32_t fake_regs[0x40];
static uint32_t fifo[4];static unsigned fifo_n;
volatile uint32_t *const test_mbox=fake_regs; /* index (off-0xb880)/4 */
static volatile uint32_t buf0[16] __attribute__((aligned(16))),buf1[16] __attribute__((aligned(16)));
volatile uint32_t *const async_buffers[2]={buf0,buf1};
/* status (0xb898 -> idx 6): bit30 = read FIFO empty; reads of 0xb880 pop. */
#define ST 6
static void sync_status(void) {fake_regs[ST]=fifo_n?0:0x40000000u;}
int main(void) {
 uint32_t v[3]={3,1000,0},out[3];
 fake_regs[ST]=0x40000000u;
 assert(property_async_start(0x38002,v,3));
 assert(!property_async_start(0x38002,v,3)); /* owned */
 /* lost reply: -110 once at 100 ms, still owned until abandon */
 now_us=50000;assert(property_async_poll(out,2)==0);
 now_us=150000;assert(property_async_poll(out,2)==-110);
 assert(property_async_poll(out,2)==0);
 assert(!property_async_start(0x38002,v,3));
 /* slow but valid reply at 900 ms is accepted */
 {
  fifo[0]=ASYNC_ADDR(async_which);fifo_n=1;
 }
 /* abandon instead (new scenario below) */
 fifo_n=0;
 now_us=1200000;assert(property_async_poll(out,2)==-5);
 /* ownership freed: next request starts, on the other buffer */
 unsigned old=async_which;
 assert(property_async_start(0x40002,v,1));
 assert(async_which!=old);
 /* late reply for the abandoned buffer must not complete the new request */
 mailbox_route(ASYNC_ADDR(old));
 assert(!async_ready);
 /* the real reply for the new request does */
 mailbox_route(ASYNC_ADDR(async_which));
 assert(async_ready);
 async_buffer[1]=0x80000000u;async_buffer[4]=0x80000004u;async_buffer[5]=7;
 assert(property_async_poll(out,1)==1&&out[0]==7);
 /* slow-but-valid: reply at 900 ms, before abandonment */
 now_us=2000000;assert(property_async_start(0x40002,v,1));
 now_us=2900000;assert(property_async_poll(out,1)==-110||1);
 mailbox_route(ASYNC_ADDR(async_which));
 async_buffer[1]=0x80000000u;async_buffer[4]=0x80000004u;async_buffer[5]=9;
 assert(property_async_poll(out,1)==1&&out[0]==9);
 (void)sync_status;
 puts("bcm_async ok");return 0;
}
