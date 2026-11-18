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
/* Isolated host-tested subset, NOT wired into USB. 0 on success; -1 rejects
 * malformed/truncated input or a capability overflow: >24 nonconstant nonzero
 * fields TOTAL (array slots count too), >8 distinct IDs with Input items
 * (including implicit ID 0), Report Size >32, Report Count >65535 even when
 * size=0, or cumulative bits >65535 per ID (including constant padding).
 * Global Push/Pop and usage/page values >16 bits are explicitly unsupported
 * and rejected, not interpreted via truncation. Other unknown items, Output,
 * Feature and collection semantics are ignored; this is not a full HID validator.
 * Local usage lists retain at most 24 entries; arrays use only the first usage
 * or range minimum. Only decode a table after parse returned 0: error output
 * may be partial. No general keyboard/NKRO or full usage-range semantics. */
int hidrep_parse(const uint8_t *desc,uint32_t len,struct hidrep *out);
/* Decodes one received report (ID byte first when the descriptor uses IDs). 0, or -1 when the
 * report is short or its ID is unknown. Consumer/system output retains the first
 * four active usages per page in field order; additional active usages are
 * deliberately truncated, not a complete pressed-state set. Buttons cover 1..32;
 * motion covers Generic Desktop X/Y/wheel only. */
int hidrep_decode(const struct hidrep *h,const uint8_t *rep,uint32_t len,struct hidrep_out *o);
#endif
