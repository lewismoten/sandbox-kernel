/* SPDX-License-Identifier: MIT; fake block media and CDC, real core logic. */
#include "host_exports.h"
#include "kernel.h"
#include "tusb.h"
#include <stdio.h>
#include <stdlib.h>
static FILE *disk;
static uint32_t now,writes,fail_after=~0u;
static uint8_t input[MAX_IMAGE+1024];static unsigned in_r,in_w;
static char output[8192];static unsigned out;
bool storage_ready,updates_ready;uint32_t boot_slot,boot_generation=1;
const char *last_error="none";
const char *sd_fail="none";
uint32_t micros(void) {return now;}
void advance(uint32_t us) {now+=us;}
uint32_t write_count(void) {return writes;}
void inject_failure(uint32_t after) {fail_after=after;}
void open_disk(const char *path) {disk=fopen(path,"r+b");if(!disk)abort();}
void close_disk(void) {fclose(disk);}
bool sd_init(void) {return disk!=NULL;}
bool sd_init_after_handoff(const uint32_t in[8]) {(void)in;return disk!=NULL;}
static uint32_t media_sectors=264192;
void set_media_sectors(uint32_t sectors) {media_sectors=sectors;}
uint32_t sd_sectors(void) {return media_sectors;}
static uint32_t reads;uint32_t read_count(void) {return reads;}
static bool pump_read;
static bool nested_run;static int nested_result;
void probe_run_on_read(void) {nested_run=true;}
int probe_run_result(void) {return nested_result;}
extern int os_start(const char *path);
static uint32_t read_fail_lba=~0u;
void fail_read_sector(uint32_t lba) {read_fail_lba=lba;}
/* One transient failure: the nth read of lba from now fails, then it reads normally. */
static uint32_t nth_lba=~0u,nth_left;
void fail_read_nth(uint32_t lba,uint32_t n) {nth_lba=lba;nth_left=n;}
static bool nth_fails(uint32_t lba) {return lba==nth_lba&&nth_left&&!--nth_left;}
void protocol_on_next_sd_read(void) {pump_read=true;}
/* The board's SD driver calls service_io() (usb_task, protocol_task) inside every wait of a transfer. With
   protocol_on_every_sd_op(1) each outermost read and write does the same here; an SD call that begins while
   another one is in flight is counted (on the board it would send a command into a transfer in progress). */
uint32_t sd_depth;static bool pump_every;static uint32_t nested_ops;
void protocol_on_every_sd_op(int on) {pump_every=on!=0;}
uint32_t nested_sd_ops(void) {return nested_ops;}
static void sd_enter(void) {if(sd_depth++)nested_ops++;else if(pump_every)protocol_task();}
bool sd_read(uint32_t lba,void *p) {reads++;sd_enter();if(nested_run){nested_run=false;nested_result=os_start("/OS/DIAG.ELF");}if(pump_read){pump_read=false;protocol_task();}bool ok=lba!=read_fail_lba&&!nth_fails(lba)&&fseek(disk,(long)lba*512,SEEK_SET)==0&&fread(p,1,512,disk)==512;sd_depth--;return ok;}
static uint32_t once_at=~0u;void fail_write_once(uint32_t after) {once_at=writes+after;} /* the write `after` from now fails, once */
static bool write_raw(uint32_t lba,const void *p) {if(writes==once_at) {once_at=~0u;writes++;return false;}if(writes++>=fail_after)return false;return fseek(disk,(long)lba*512,SEEK_SET)==0&&fwrite(p,1,512,disk)==512&&fflush(disk)==0;}
bool sd_write(uint32_t lba,const void *p) {sd_enter();bool ok=write_raw(lba,p);sd_depth--;return ok;}
void error(const char *s) {last_error=s;}
void reboot(void) {abort();}
void usb_reply_bytes(const uint8_t *p,uint32_t n) {if(out+n>=sizeof(output))abort();memcpy(output+out,p,n);out+=n;}
unsigned output_size(void) {return out;}
void usb_reply(const char *s) {size_t n=strlen(s);if(out+n>=sizeof(output))abort();memcpy(output+out,s,n+1);out+=n;}
bool usb_reply_pending(void) {return false;}
const char *take_output(void) {output[out]=0;out=0;return output;}
void feed(const void *data,unsigned n) {if(in_w+n>sizeof(input))abort();memcpy(input+in_w,data,n);in_w+=n;}
uint32_t tud_cdc_available(void) {return in_w-in_r;}
uint32_t tud_cdc_read(void *p,uint32_t n) {if(n>in_w-in_r)n=in_w-in_r;memcpy(p,input+in_r,n);in_r+=n;return n;}
void tud_cdc_read_flush(void) {in_r=in_w=0;}
bool initialize(void) {storage_ready=store_init();updates_ready=storage_ready&&verify_confirmed();return updates_ready;}
/* M1 slice 2: the kernel map, framebuffer, logs and watchdog seen by
   core/uspace.c, core/sys.c, core/os.c and core/fatput.c. */
#include "fb.h"
#include "shot.h"
#ifndef PANEL_PROBE
bool panel_snapshot(uint8_t out[4000]) {for(unsigned i=0;i<4000;i++)out[i]=(uint8_t)i;return true;}
uint32_t panel_snapshot_order(void) {return 1;}
#endif
uint32_t fb_mode_generation;
struct fb_info fb0;uint8_t *fb_pixels;uint32_t boot_trial;
static uint32_t ktable[4096];
const uint32_t *mmu_table(void) {return ktable;}
uint32_t *kernel_table(void) {return ktable;}
void watchdog_feed(void) {}
static char logs[1<<16];static unsigned logn;
void log_line(const char *s) {size_t n=strlen(s);if(logn+n+2>=sizeof(logs))logn=0;memcpy(logs+logn,s,n);logn+=(unsigned)n;logs[logn++]='\n';logs[logn]=0;}
const char *take_log(void) {static char copy[1<<16];memcpy(copy,logs,logn+1);logn=0;logs[0]=0;return copy;}
static uint8_t hw[3900*540]; /* the kernel's 960x540x32 screen, pitch padded */
uint8_t *fb_setup(uint32_t order,int present) {
 fb_mode_generation++;memset(&fb0,0,sizeof fb0);fb_pixels=NULL;if(!present)return NULL;
 fb0.width=640;fb0.height=480;fb0.depth=32;fb0.pitch=2700;fb0.bytes=sizeof(hw);fb0.pixel_order=order;
 memset(hw,0xaa,sizeof hw);fb_pixels=hw;return hw;
}
/* ABI 1.2 mode sets on the host: the fake buffer takes the mode (pitch padded
   as the board's is), and palettes are kept for the tests to read. */
uint32_t fb_palette[256],fb_mode_sets;
static bool stop_on_mode;
void probe_stop_on_mode(void) {stop_on_mode=true;}
extern void os_request_stop(void);
int fb_set_mode(uint32_t w,uint32_t h,uint32_t d) {
 if(stop_on_mode) {stop_on_mode=false;os_request_stop();}
 if(!fb_pixels) return -19;
 uint32_t pitch=w*(d/8u)+60u; if(pitch*h>sizeof hw) return -5;
 fb0.width=w;fb0.height=h;fb0.depth=d;fb0.pitch=pitch;memset(hw,0,sizeof hw);fb_mode_sets++;fb_mode_generation++;return 0;
}
int fb_set_palette(uint32_t first,uint32_t count,const uint32_t *rgb) {memcpy(fb_palette+first,rgb,count*4u);for(uint32_t i=0;i<count;i++) {fb_shot_palette[first+i]=rgb[i];fb_shot_palette_known[first+i]=true;}return 0;}
