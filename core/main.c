/* SPDX-License-Identifier: MIT */
#include "kernel.h"
#include "fb.h"
#include "os.h"
#include "autostart.h"
#include "clock.h"
#include "power.h"
#include "sys.h"
#include "input.h"
#include "fb_art.h"
#include "audio_null.h"
#include "hdmi_audio.h"
#include "usbrole.h"
#include "assets.h"
#include "bootrec.h"
#include "usblog.h"
#if USB_ROLE_HOST
#include "usbkbd.h"
#endif
bool storage_ready,updates_ready;
uint32_t boot_slot,boot_generation,boot_trial;
const char *last_error="none";
void log_line(const char *s) {console_log(s);uart_puts(s);uart_puts("\r\n");usb_log(s);}
void error(const char *s) {last_error=s;log_line(s);}
void kernel_main(void) {
 uint32_t boot_time=micros();
 uint32_t h[8];memcpy(h,(const void *)0x7000,sizeof(h));
 if(h[0]!=HANDOFF_MAGIC||h[1]!=1||h[2]>1||h[3]>BOOT_COMPANION||h[6]<128*1024*1024u||h[7]!=crc32(h,28)) {uart_init();uart_puts("Invalid selector handoff\r\n");for(;;){}}
 boot_slot=h[2];boot_generation=h[4];boot_trial=h[3]==1;
#if USB_ROLE_HOST
 /* R1: a host-role image runs only as a consumed trial or a boot-only companion. Entered any other way (a confirmed or
    metadata-lost boot) it does nothing: no USB, no confirmation. The watchdog the selector armed
    resets the board every few seconds, and the UART names the reason. */
 if(!h[3]) {uart_init();uart_puts("Host-role image is not on trial or companion handoff; not running. Restore the device kernel (card reader).\r\n");for(;;){}}
#endif
 /* The first visible image is completed before USB, SD or timer IRQ setup.
    System-timer free-running hardware is used only for bounded panel delays. */
 input_init(); /* R3: before USB or SD can deliver a keyboard report; the OS-facing note stays below */
 panel_start();while(!panel_boot_done())panel_task("starting",0);
 mmu_init();uart_init();irq_init();
#if !USB_ROLE_HOST
 usb_init();
#endif
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
#if USB_ROLE_HOST
 if(h[3]==BOOT_COMPANION&&(!updates_ready||control[4]!=NO_SLOT||boot_slot==control[3]||control[9]!=boot_generation)) {
  error("Companion metadata mismatch");for(;;){}
 }
 usb_init();
 if(storage_ready) {asset_mount();bootrec_boot(BD_KEYBOARD,false);}
#else
 /* Decide before framebuffer/audio/OS startup: no running DMA or program to
    quiesce. ID is latched at boot; a live device link always cancels intent. */
 if(!boot_trial&&updates_ready&&boot_slot==control[3]) {
  (void)usb_host_wanted(); /* anchor the debounce before any USB task work */
  uint32_t since=micros();
  enum usb_companion_action action=USB_COMPANION_NONE;
  /* Keep asking for USBROLE_DECISION_WINDOW_US (well past the debounce) while the ID pin says an adapter is
     there and no Mac link has come up. No adapter, or a Mac link, is decided at once. */
  do {
   usb_task(); /* no protocol commands during the boot decision window */
   if(usb_mounted()||!usb_id_grounded()) break;
   enum usb_role intent=usb_host_wanted()?USB_ROLE_HOST_WANTED:USB_ROLE_DEVICE;
   action=usbrole_companion_action(intent,control[9]!=0,true);
   if(action!=USB_COMPANION_NONE) break;
  } while(!expired(since,USBROLE_DECISION_WINDOW_US));
  enum boot_decision d=usb_mounted()?BD_LINK:!usb_id_grounded()?BD_NO_ADAPTER:action==USB_COMPANION_HANDOFF?BD_KEYBOARD:action==USB_COMPANION_MISSING?BD_INSTALL_KBD:BD_NO_ADAPTER;
  log_line(usb_id_grounded()?"OTG ID pin: grounded (adapter)":"OTG ID pin: floating (no adapter)");
  if(storage_ready) {asset_mount();bootrec_boot(d,true);}
  usb_set_boot_note(bootinfo_status(d));
  {char t[40]="Boot decision: ";const char *w=bootinfo_decision_name(d);memcpy(t+15,w,strlen(w)+1);log_line(t);}
  if(action==USB_COMPANION_MISSING||(action==USB_COMPANION_HANDOFF&&!host_companion_launch()))
   log_line("OTG: keyboard kernel missing or invalid; install host companion over Mac USB");
 } else if(storage_ready) {asset_mount();bootrec_boot(BD_NONE,false);}
#endif
 /* M1 probes: HDMI framebuffer and read-only assets. Neither touches USB,
    the panel or any card sector; failures are logged, never fatal. */
 /* Slice 3: the synthetic input device (USB KEY/AXIS), present from boot. */
 boot_note("INPUT /dev/input/event0 synthetic (USB KEY, AXIS)");
 watchdog_feed();cpu_speed();fb_start();hdmi_audio_init();cec_hw_init(); /* after the firmware's HDMI is up; falls back to the null backend */
 if(storage_ready)asset_probe();
 watchdog_feed();
 /* M1: the OS program starts 10 s after boot, so a program that could take
    the kernel down can still be replaced over USB (PUTF) before it runs.
    GMOS.ELF if present (or AUTORUN.TXT's choice), else DIAG.ELF; a rejected
    file falls back to DIAG.ELF, and a missing DIAG keeps the recovery console. */
 bool autostart=boot_storage_policy(storage_ready);
 if(autostart) {char t[48]="OS autostart in 10 s: ";const char *a=autostart_pick();memcpy(t+22,a,strlen(a)+1);log_line(t);}
 power_hw_init();
 uint32_t start=micros(),last=boot_time,old_ticks=ticks,seconds=0,remainder=0;
 bool trial=h[3]==1&&control[0]==CONTROL_MAGIC&&control[4]==boot_slot&&control[5]&&control[7]==boot_generation;
 uint32_t healthy=micros(),diagnostics_start=micros();bool health_failed=false;
#if USB_ROLE_HOST
 unsigned kbd_seen=0,kbd_saved=0;uint32_t kbd_since=micros();
#endif
 unsigned usb_saves=0;bool usb_dirty=false;uint32_t usb_ver=0,usb_since=micros();
 if(h[3]==1&&!trial) {error("Trial metadata mismatch");for(;;)service_io();}
 for(;;) {
  clock_us();audio_null_task();usb_task();protocol_task();protocol_card_task();os_task();cec_hw_task();
#if USB_ROLE_HOST
  /* the keyboards found, saved once the count has held for 2 s (a hub enumerates one device at a time) */
  if(usbkbd_count()!=kbd_seen) {kbd_seen=usbkbd_count();kbd_since=micros();}
  else if(kbd_seen!=kbd_saved&&expired(kbd_since,2000000)) {if(storage_ready&&!os_running()) {kbd_saved=kbd_seen;char t[28]="Keyboards found: ";t[17]=(char)('0'+(kbd_seen>9?9:kbd_seen));t[18]=0;log_line(t);bootrec_keyboards(kbd_seen);}}
#endif
  /* USB.TXT: saved once the log has been quiet for 1.5 s (at most 12 times a boot, and not once the OS runs) */
  if(usblog_version()!=usb_ver) {usb_ver=usblog_version();usb_since=micros();usb_dirty=true;}
  else if(usb_dirty&&usb_saves<12&&expired(usb_since,1500000)&&storage_ready&&!os_running()) {
   usb_saves++;usb_dirty=false;uint32_t a,r;
#if USB_ROLE_HOST
   usb_host_events(&a,&r);
#else
   a=r=0;
#endif
   bootrec_usblog(a,r);usb_ver=usblog_version();
  }
  if(autostart_cancelled()) autostart=false;
  if(autostart&&expired(boot_time,10000000)&&!protocol_busy()) {
   autostart=false;
   autostart_launch();
  }
  uint32_t now=micros(),delta=now-last;last=now;remainder+=delta;while(remainder>=1000000) {seconds++;remainder-=1000000;}
  panel_task(usb_state(),seconds);fb_recovery_task();
  if(expired(start,100000)) {
   if(ticks==old_ticks) {error("Timer progress lost");for(;;)service_io();}
   old_ticks=ticks;start=micros();watchdog_feed();
  }
  if(trial&&!protocol_busy()&&expired(healthy,1000000)) {
   uint32_t c[128];healthy=micros();
   if(!storage_ready||!updates_ready||!usb_healthy()||!control_healthy())health_failed=true;
   /* Read and compare the authoritative record before trial confirmation. */
   if(health_failed) {error("Trial storage health failed");for(;;)service_io();}
#if USB_ROLE_HOST
   /* A host-role image has no Mac link to replace it, so it never confirms: it stays a
      trial, and a power cycle returns to the confirmed device-role kernel. */
   (void)c;
   if(expired(diagnostics_start,30000000)) {trial=false;log_line("Host-role test image: not confirmed; power off and on to return to the device kernel");}
#else
   if(expired(diagnostics_start,30000000)) {
    memcpy(c,control,512);c[3]=boot_slot;c[6]=boot_generation;c[4]=NO_SLOT;c[5]=0;c[7]=0;memcpy(c+16,c+24,32);memset(c+24,0,32);
    if(!control_commit(c)) {error("Confirmation failed");for(;;)service_io();}
    trial=false;boot_trial=0;log_line("Trial confirmed after 30 seconds");
   }
#endif
  }
  power_hw_task();
  /* SD is synchronous: no request survives this boundary. USB IRQs and DMA
     wake directly; the <=1 ms one-shot bounds CEC, USB deferred retries,
     null audio, panel and all other polled work. Never park a runnable OS
     or its incremental cleanup. Recheck sleep after the work above. */
  if(!os_cleanup_pending()&&!os_stop_pending()&&(!os_running()||os_asleep()))
   idle_wait(power_wait_us(clock_us(),sys_wake_us));
 }
}
