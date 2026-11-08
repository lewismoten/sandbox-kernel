/* SPDX-License-Identifier: MIT */
#include "kernel.h"
#include "fb.h"
#include "os.h"
#include "autostart.h"
#include "clock.h"
#include "input.h"
void asset_probe(void);
bool storage_ready,updates_ready;
uint32_t boot_slot,boot_generation,boot_trial;
const char *last_error="none";
void log_line(const char *s) {uart_puts(s);uart_puts("\r\n");usb_log(s);}
void error(const char *s) {last_error=s;log_line(s);}
extern bool panel_boot_done(void),verify_confirmed(void),protocol_busy(void),usb_healthy(void),control_healthy(void);
void kernel_main(void) {
 uint32_t boot_time=micros();
 uint32_t h[8];memcpy(h,(const void *)0x7000,sizeof(h));
 if(h[0]!=HANDOFF_MAGIC||h[1]!=1||h[2]>1||h[3]>1||h[6]<128*1024*1024u||h[7]!=crc32(h,28)) {uart_init();uart_puts("Invalid selector handoff\r\n");for(;;){}}
 boot_slot=h[2];boot_generation=h[4];boot_trial=h[3];
 /* The first visible image is completed before USB, SD or timer IRQ setup.
    System-timer free-running hardware is used only for bounded panel delays. */
 panel_start();while(!panel_boot_done())panel_task("starting",0);
 mmu_init();uart_init();irq_init();usb_init();
 log_line("sandbox-kernel " BUILD_ID " Zero W v1.1 RASPPI=1");
 char num[12];number(num,h[5]);log_line("Reset cause (PM_RSTS decimal):");log_line(num);
 number(num,boot_slot);log_line("Selected slot:");log_line(num);
 number(num,boot_generation);log_line("Generation:");log_line(num);
 watchdog_feed();
 {static uint32_t sdb[8];memcpy(sdb,(const void *)0x7040,32);sd_handoff_block=sdb;}
 storage_ready=store_init();
 if(!storage_ready) {log_line("SD/FAT unavailable, step:");error(store_fail);if(strcmp(property_fail,"none"))log_line(property_fail);}
 else {
  updates_ready=verify_confirmed();
  if(!updates_ready)error("No verified fallback");
  if(!h[3]&&control[0]==CONTROL_MAGIC&&control[4]!=NO_SLOT) {
   uint32_t c[128];memcpy(c,control,512);c[4]=NO_SLOT;c[5]=0;c[7]=0;memset(c+24,0,32);
   if(!control_commit(c)) {updates_ready=false;error("Fallback commit failed");}
  }
 }
 /* M1 probes: HDMI framebuffer and read-only assets. Neither touches USB,
    the panel or any card sector; failures are logged, never fatal. */
 /* Slice 3: the synthetic input device (USB KEY/AXIS), present from boot. */
 input_init();boot_note("INPUT /dev/input/event0 synthetic (USB KEY, AXIS)");
 watchdog_feed();cpu_speed();fb_start();
 if(storage_ready)asset_probe();
 watchdog_feed();
 /* M1: the OS program starts 10 s after boot, so a program that could take
    the kernel down can still be replaced over USB (PUTF) before it runs.
    GMOS.ELF if present (or AUTORUN.TXT's choice), else DIAG.ELF; a rejected
    file falls back to DIAG.ELF, and a missing DIAG keeps the recovery console. */
 bool autostart=storage_ready;
 if(autostart) {char t[48]="OS autostart in 10 s: ";const char *a=autostart_pick();memcpy(t+22,a,strlen(a)+1);log_line(t);}
 uint32_t start=micros(),last=boot_time,old_ticks=ticks,seconds=0,remainder=0;
 bool trial=h[3]&&control[0]==CONTROL_MAGIC&&control[4]==boot_slot&&control[5]&&control[7]==boot_generation;
 uint32_t healthy=micros(),diagnostics_start=micros();bool health_failed=false;
 if(h[3]&&!trial) {error("Trial metadata mismatch");for(;;)service_io();}
 for(;;) {
  clock_us();usb_task();protocol_task();os_task();
  if(autostart_cancelled()) autostart=false;
  if(autostart&&expired(boot_time,10000000)&&!protocol_busy()) {
   autostart=false;
   if(!os_running()&&autostart_run(autostart_pick()))log_line("OS none; recovery console (USB PUTF and RUN available)");
  }
  uint32_t now=micros(),delta=now-last;last=now;remainder+=delta;while(remainder>=1000000) {seconds++;remainder-=1000000;}
  panel_task(usb_state(),seconds);
  if(expired(start,100000)) {
   if(ticks==old_ticks) {error("Timer progress lost");for(;;)service_io();}
   old_ticks=ticks;start=micros();watchdog_feed();
  }
  if(trial&&!protocol_busy()&&expired(healthy,1000000)) {
   uint32_t c[128];healthy=micros();
   if(!storage_ready||!updates_ready||!usb_healthy()||!control_healthy())health_failed=true;
   /* Read and compare the authoritative record before trial confirmation. */
   if(health_failed) {error("Trial storage health failed");for(;;)service_io();}
   if(expired(diagnostics_start,30000000)) {
    memcpy(c,control,512);c[3]=boot_slot;c[6]=boot_generation;c[4]=NO_SLOT;c[5]=0;c[7]=0;memcpy(c+16,c+24,32);memset(c+24,0,32);
    if(!control_commit(c)) {error("Confirmation failed");for(;;)service_io();}
    trial=false;boot_trial=0;log_line("Trial confirmed after 30 seconds");
   }
  }
 }
}
