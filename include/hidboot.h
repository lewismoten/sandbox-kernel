/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef HIDBOOT_H
#define HIDBOOT_H
#include <stdint.h>
#include "input.h"
/* HID boot-protocol keyboard report (8 bytes: modifiers, reserved, 6 key usages) diffed
 * against the previous report into key down/up changes using Linux key codes (the codes
 * /dev/input/event0 carries). Pure logic apart from hidboot_feed, which calls input_frame. */
#define HIDBOOT_REPORT 8u
#define HIDBOOT_CHANGES_MAX 20u   /* 8 modifiers + 6 releases + 6 presses */
struct hidboot { uint8_t prev[HIDBOOT_REPORT]; };
void hidboot_init(struct hidboot *s);
/* Linux key code for a HID keyboard usage (page 7), or 0 when unmapped. */
uint16_t hidboot_linux_key(uint8_t usage);
/* Changes the report makes (releases first), at most HIDBOOT_CHANGES_MAX. Returns the count,
 * or a negative value when the report is not 8 bytes. A rollover-error report (any key 0x01) is
 * ignored: it returns 0 and keeps the previous state. */
int hidboot_diff(struct hidboot *s,const uint8_t *rep,uint32_t len,struct input_change *out);
/* hidboot_diff, then input_frame in frames of INPUT_FRAME_MAX. Returns 0 or a negative errno. */
int hidboot_feed(struct hidboot *s,const uint8_t *rep,uint32_t len);
/* Every Linux key code (below 128) the 8-byte report holds down, as bits in out[16]; modifiers included. */
void hidboot_held(const uint8_t *rep,uint8_t out[16]);
/* All keys up (device removed or reset): releases whatever is held. Same return as hidboot_feed. */
int hidboot_release_all(struct hidboot *s);
#endif
