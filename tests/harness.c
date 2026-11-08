/* SPDX-License-Identifier: MIT; fake block media and CDC, real core logic. */
#include "kernel.h"
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
static uint32_t read_fail_lba=~0u;
void fail_read_sector(uint32_t lba) {read_fail_lba=lba;}
void protocol_on_next_sd_read(void) {pump_read=true;}
bool sd_read(uint32_t lba,void *p) {reads++;if(pump_read){pump_read=false;protocol_task();}return lba!=read_fail_lba&&fseek(disk,(long)lba*512,SEEK_SET)==0&&fread(p,1,512,disk)==512;}
bool sd_write(uint32_t lba,const void *p) {if(writes++>=fail_after)return false;return fseek(disk,(long)lba*512,SEEK_SET)==0&&fwrite(p,1,512,disk)==512&&fflush(disk)==0;}
void error(const char *s) {last_error=s;}
void reboot(void) {abort();}
void usb_reply(const char *s) {size_t n=strlen(s);if(out+n>=sizeof(output))abort();memcpy(output+out,s,n+1);out+=n;}
bool usb_reply_pending(void) {return false;}
const char *take_output(void) {output[out]=0;out=0;return output;}
void feed(const void *data,unsigned n) {if(in_w+n>sizeof(input))abort();memcpy(input+in_w,data,n);in_w+=n;}
uint32_t tud_cdc_available(void) {return in_w-in_r;}
uint32_t tud_cdc_read(void *p,uint32_t n) {if(n>in_w-in_r)n=in_w-in_r;memcpy(p,input+in_r,n);in_r+=n;return n;}
void tud_cdc_read_flush(void) {in_r=in_w=0;}
bool verify_confirmed(void);
bool initialize(void) {storage_ready=store_init();updates_ready=storage_ready&&verify_confirmed();return updates_ready;}
/* M1 slice 2: the kernel map, framebuffer, logs and watchdog seen by
   core/uspace.c, core/sys.c, core/os.c and core/fatput.c. */
#include "fb.h"
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
 memset(&fb0,0,sizeof fb0);fb_pixels=NULL;if(!present)return NULL;
 fb0.width=640;fb0.height=480;fb0.depth=32;fb0.pitch=2700;fb0.bytes=sizeof(hw);fb0.pixel_order=order;
 memset(hw,0xaa,sizeof hw);fb_pixels=hw;return hw;
}
/* ABI 1.2 mode sets on the host: the fake buffer takes the mode (pitch padded
   as the board's is), and palettes are kept for the tests to read. */
uint32_t fb_palette[256],fb_mode_sets;
int fb_set_mode(uint32_t w,uint32_t h,uint32_t d) {
 if(!fb_pixels) return -19;
 uint32_t pitch=w*(d/8u)+60u; if(pitch*h>sizeof hw) return -5;
 fb0.width=w;fb0.height=h;fb0.depth=d;fb0.pitch=pitch;memset(hw,0,sizeof hw);fb_mode_sets++;return 0;
}
int fb_set_palette(uint32_t first,uint32_t count,const uint32_t *rgb) {memcpy(fb_palette+first,rgb,count*4u);return 0;}
