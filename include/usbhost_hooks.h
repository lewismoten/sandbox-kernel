/* SPDX-License-Identifier: MIT; local TinyUSB host overlay diagnostics. */
#ifndef USBHOST_HOOKS_H
#define USBHOST_HOOKS_H
void tuh_control_retry_cb(uint8_t daddr,const tusb_control_request_t *request,
                          xfer_result_t result,unsigned retry,unsigned delay_ms);
void tuh_enumeration_result_cb(uint8_t daddr,uint8_t hub,uint8_t port,bool success);
void tuh_hub_retry_cb(uint8_t hub,uint8_t port,unsigned retry,const char *outcome);
#endif
