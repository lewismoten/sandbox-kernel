/* SPDX-License-Identifier: MIT; bounded host recovery, no hardware dependencies. */
#ifndef USBHOST_RETRY_H
#define USBHOST_RETRY_H
#include <stdbool.h>
#include <stdint.h>
#define USBHOST_CONTROL_RETRIES 3
#define USBHOST_PORT_RETRIES 2
static inline uint32_t usbhost_backoff(unsigned retry) {return 20u << (retry-1);}
static inline bool usbhost_due(uint32_t now,uint32_t deadline) {return (int32_t)(now-deadline)>=0;}
/* Keep every bit until the shared enumeration/control state is available. */
static inline uint8_t usbhost_change_take(uint8_t *pending,bool busy) {
 if(busy||!*pending)return 0;
 uint8_t bit=(uint8_t)(*pending & (uint8_t)(0u-*pending));
 *pending &= (uint8_t)~bit;return bit;
}
#endif
