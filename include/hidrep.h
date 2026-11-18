/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * HID report-protocol decoding for the second interface of a keyboard dongle: a mouse or
 * touchpad, system control and consumer control (media keys). The report descriptor is read
 * once into a field list; each report is then decoded from it. No hardware, no allocation. */
#ifndef HIDREP_H
#define HIDREP_H
#include <stdint.h>
#define HIDREP_FIELDS 24
#define HIDREP_ACTIVE 4
struct hidrep_field {uint8_t id,bits,sign,array; uint16_t off,page,usage; int32_t lmin;};
struct hidrep {uint8_t uses_ids,n; struct hidrep_field f[HIDREP_FIELDS];};
struct hidrep_out {
 int32_t dx,dy,wheel;            /* relative motion in this report */
 uint32_t buttons;               /* bit 0 = button 1 */
 uint16_t consumer[HIDREP_ACTIVE]; /* usages held on page 0x0C; 0 = unused slot */
 uint16_t system[HIDREP_ACTIVE];   /* usages held on page 0x01 (0x81 power down, 0x82 sleep, 0x83 wake) */
 uint8_t id;                     /* the report ID, 0 when the descriptor has none */
};
/* Reads a report descriptor. 0, or -1 for a truncated or malformed one. Fields past the table
 * are dropped (the caller still gets the ones that fit). */
int hidrep_parse(const uint8_t *desc,uint32_t len,struct hidrep *out);
/* Decodes one received report (ID byte first when the descriptor uses IDs). 0, or -1 when the
 * report is short or its ID is unknown. */
int hidrep_decode(const struct hidrep *h,const uint8_t *rep,uint32_t len,struct hidrep_out *o);
#endif
