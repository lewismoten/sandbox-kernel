/* SPDX-License-Identifier: MIT; the input event queue behind /dev/input/event0
 * (acceptance test 8). Linux input conventions, declared independently: 16-byte
 * records {u64 monotonic ns, u16 type, u16 code, i32 value}; a source reports a
 * frame of key/axis changes and the queue closes it with SYN_REPORT; events
 * that change nothing are dropped (as the Linux input core does); a frame that
 * does not fit empties the queue and leaves one SYN_DROPPED, after which the
 * reader asks for the current state (that query also empties the queue, so no
 * change is seen twice). Removal ends every open handle with ENODEV; a device
 * added again is a new instance, so an old handle never reads it.
 * The only source in slice 3 is synthetic: USB KEY/AXIS commands. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define INPUT_QUEUE 64u      /* records per open handle */
#define INPUT_KEYS 768u      /* key and button codes 0..767 (Linux KEY_MAX 0x2ff) */
#define INPUT_AXES 32u       /* absolute axis codes 0..31 */
#define INPUT_AXIS_MIN (-32768)
#define INPUT_AXIS_MAX 32767
#define INPUT_FRAME_MAX 16u  /* changes in one frame */
#define EV_SYN 0u
#define EV_KEY 1u
#define EV_ABS 3u
#define SYN_REPORT 0u
#define SYN_DROPPED 3u
#define INPUT_SYNTHETIC 1u   /* query flag: events come from USB commands, not a device */
struct input_event_rec {uint64_t ns; uint16_t type,code; int32_t value;};
struct input_change {uint16_t type,code; int32_t value;};
void input_init(void);            /* the synthetic device, present */
bool input_present(void);
uint32_t input_instance(void);    /* changes on every add */
void input_add(void);
void input_remove(void);
/* One frame from the source: 0, or a negative errno (nothing is applied then). */
int input_frame(const struct input_change *c,unsigned n);
int input_open(uint32_t instance);   /* 0, -ENODEV, -EBUSY (one reader at a time) */
void input_close(uint32_t instance);
/* Up to max records into out: count, -EAGAIN when empty, -ENODEV after removal. */
int input_read(uint32_t instance,struct input_event_rec *out,uint32_t max);
/* Current state: key bits (INPUT_KEYS/64 words) and axes; empties the queue. */
int input_state(uint32_t instance,uint64_t keys[INPUT_KEYS/64],int64_t axes[INPUT_AXES]);
extern uint32_t input_dropped; /* overflow count since boot */

extern uint32_t input_activity; /* accepted changes, even without a reader */
