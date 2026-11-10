/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* CEC User Control codes to Linux-style EV_KEY codes, with repeat and release.
 * Pure C, clock injected. Emits key events only; the caller adds one EV_SYN
 * SYN_REPORT after each call that returned events. */
#ifndef CEC_KEYS_H
#define CEC_KEYS_H
#include <stdint.h>

#define CEC_KEY_RELEASE_MS 550u  /* CEC's rule: no Pressed for ~550 ms ends the press */
#define CEC_KEYS_MAX_EVENTS 3u

#define CEC_UI_SELECT 0x00u
#define CEC_UI_UP 0x01u
#define CEC_UI_DOWN 0x02u
#define CEC_UI_LEFT 0x03u
#define CEC_UI_RIGHT 0x04u
#define CEC_UI_EXIT 0x0Du        /* the spec's Exit; Linux's header names it BACK */

struct cec_key_event { uint16_t code; uint8_t value; }; /* value 0 up, 1 down, 2 repeat */

struct cec_keys {
    uint16_t held;      /* Linux key code, 0 when none */
    uint8_t ui;         /* the UI command that is held */
    uint32_t last_ms;   /* time of the latest Pressed for it */
};

void cec_keys_init(struct cec_keys *k);
/* Linux key code for a UI command, or 0 when the command is not mapped. */
uint16_t cec_keys_map(uint8_t ui);
/* Each returns the number of events written to ev (room for CEC_KEYS_MAX_EVENTS). */
unsigned cec_keys_pressed(struct cec_keys *k, uint8_t ui, uint32_t now_ms, struct cec_key_event *ev);
unsigned cec_keys_released(struct cec_keys *k, struct cec_key_event *ev);
unsigned cec_keys_tick(struct cec_keys *k, uint32_t now_ms, struct cec_key_event *ev);
#endif
