/* SPDX-License-Identifier: MIT; fake TinyUSB transport, not USB qualification. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct {
 uint8_t bLength,bDescriptorType; uint16_t bcdUSB;
 uint8_t bDeviceClass,bDeviceSubClass,bDeviceProtocol,bMaxPacketSize0;
 uint16_t idVendor,idProduct,bcdDevice;
 uint8_t iManufacturer,iProduct,iSerialNumber,bNumConfigurations;
} tusb_desc_device_t;
typedef struct {
 uint8_t bLength,bDescriptorType; uint16_t bcdUSB;
 uint8_t bDeviceClass,bDeviceSubClass,bDeviceProtocol,bMaxPacketSize0,bNumConfigurations;
} tusb_desc_device_qualifier_t;
#define TUSB_DESC_DEVICE 1
#define TUSB_DESC_DEVICE_QUALIFIER 6
#define TUSB_DESC_OTHER_SPEED_CONFIG 7
#define TUSB_CLASS_MISC 0xef
#define MISC_SUBCLASS_COMMON 2
#define MISC_PROTOCOL_IAD 1
#define TUSB_SPEED_HIGH 2
#define TUD_CONFIG_DESC_LEN 9
#define TUD_CDC_DESC_LEN 66
/* Only queue code is under test, not these synthetic descriptors. */
#define TUD_CONFIG_DESCRIPTOR(...) 9,2
#define TUD_CDC_DESCRIPTOR(...) [TUD_CONFIG_DESC_LEN+TUD_CDC_DESC_LEN-1]=0
#define CFG_TUD_CDC_TX_BUFSIZE 64
int tud_speed_get(void);
void tud_int_handler(unsigned port);
bool tud_mounted(void);
bool tud_cdc_connected(void);
bool tusb_init(void);
bool tud_deinit(uint8_t port);
uint32_t tud_cdc_write_available(void);
uint32_t tud_cdc_available(void);
void tud_task(void);
void tud_cdc_write_char(char c);
void tud_cdc_write_str(const char *s);
void tud_cdc_write_flush(void);

/* Device callbacks supplied by the production USB glue under test. */
const uint8_t *tud_descriptor_device_cb(void);
const uint8_t *tud_descriptor_configuration_cb(uint8_t index);
const uint8_t *tud_descriptor_device_qualifier_cb(void);
const uint8_t *tud_descriptor_other_speed_configuration_cb(uint8_t index);
const uint16_t *tud_descriptor_string_cb(uint8_t index,uint16_t lang);
void tud_mount_cb(void);
void tud_umount_cb(void);
void tud_suspend_cb(bool remote);
void tud_resume_cb(void);
void tud_cdc_line_state_cb(uint8_t itf,bool dtr,bool rts);
void tud_event_hook_cb(uint8_t rhport,uint32_t eventid,bool in_isr);
uint32_t tusb_time_millis_api(void);
