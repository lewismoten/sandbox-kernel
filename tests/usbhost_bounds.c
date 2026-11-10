/* SPDX-License-Identifier: MIT; run the real enumeration callback before class opening. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#define USB_ROLE_HOST 1
#include "tusb_config.h"
typedef struct {uint8_t bytes[9];} tusb_desc_configuration_t;
typedef struct {uint8_t bytes[9];} tusb_desc_interface_t;
typedef struct {uint8_t bytes[7];} tusb_desc_endpoint_t;
enum {TUSB_DESC_INTERFACE=4,TUSB_DESC_ENDPOINT=5,TUSB_CLASS_HID=3};
static unsigned logs;
void log_line(const char *s) {assert(s&&*s);logs++;}
#include "usbhost-bounds.h"
static uint8_t config[]={
 9,2,59,0,2,1,0,0x80,50,
 9,4,0,0,1,3,1,1,0, 9,0x21,0x11,1,0,1,0x22,63,0, 7,5,0x81,3,8,0,1,
 9,4,1,0,1,3,0,0,0, 9,0x21,0x11,1,0,1,0x22,63,0, 7,5,0x82,3,8,0,1
};
static bool accept(void) {return tuh_enum_descriptor_configuration_cb(1,0,(const void *)config);}
int main(void) {
 assert(sizeof(config)==59&&accept()&&logs==0); /* ordinary 8-byte boot keyboard */
 config[31]=64;assert(accept()); /* exact buffer capacity */
 for(unsigned size=65;size<=1024;size=size==65?512:1024) {
  config[31]=(uint8_t)size;config[32]=(uint8_t)(size>>8);
  unsigned before=logs;assert(!accept()&&logs==before+1);
  if(size==1024)break;
 }
 config[31]=8;config[32]=0;
 config[56]=0;config[57]=2; /* oversized non-boot HID after a valid keyboard */
 unsigned before=logs;assert(!accept()&&logs==before+1);
 config[14]=0; /* skipped non-HID first interface must not stop the walk */
 assert(!accept());
 config[39]=0;assert(accept()); /* non-HID endpoints do not use HID buffers */
 config[39]=3;config[54]=2;assert(accept()); /* OUT does not use IN storage */
 config[54]=0x82;config[56]=8;config[57]=0;assert(accept());
 config[52]=0;assert(!accept()); /* malformed descriptor cannot hide a later endpoint */
 puts("HID configuration bounds ok");
}
