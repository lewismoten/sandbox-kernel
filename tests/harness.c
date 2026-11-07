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
uint32_t sd_sectors(void) {return 264192;}
bool sd_read(uint32_t lba,void *p) {return fseek(disk,(long)lba*512,SEEK_SET)==0&&fread(p,1,512,disk)==512;}
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
static uint8_t hw[2700*480];
uint8_t *fb_setup(uint32_t order,int present) {
 memset(&fb0,0,sizeof fb0);fb_pixels=NULL;if(!present)return NULL;
 fb0.width=640;fb0.height=480;fb0.depth=32;fb0.pitch=2700;fb0.bytes=sizeof(hw);fb0.pixel_order=order;
 memset(hw,0xaa,sizeof hw);fb_pixels=hw;return hw;
}
