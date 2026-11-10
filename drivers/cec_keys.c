/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
/* UI command mapping and press/repeat/release tracking for the CEC remote. */
#include "cec_keys.h"

/* Linux input key numbers (the ABI the kernel's /dev/input uses). */
enum { K_ESC = 1, K_1 = 2, K_9 = 10, K_0 = 11, K_BACKSPACE = 14, K_ENTER = 28,
       K_HOME = 102, K_UP = 103, K_PAGEUP = 104, K_LEFT = 105, K_RIGHT = 106,
       K_DOWN = 108, K_PAGEDOWN = 109, K_MENU = 139, K_F1 = 59 };

void cec_keys_init(struct cec_keys *k) { k->held = 0; k->ui = 0; k->last_ms = 0; }

uint16_t cec_keys_map(uint8_t ui) {
    switch (ui) {
    case 0x00: return K_ENTER;                   /* Select */
    case 0x01: return K_UP;
    case 0x02: return K_DOWN;
    case 0x03: return K_LEFT;
    case 0x04: return K_RIGHT;
    case 0x09: return K_HOME;                    /* Root menu */
    case 0x0A: return K_MENU;                    /* Setup menu */
    case 0x0D: return K_ESC;                     /* Exit / Back */
    case 0x20: return K_0;                       /* Number 0 */
    case 0x2B: return K_ENTER;                   /* Enter */
    case 0x2C: return K_BACKSPACE;               /* Clear */
    case 0x30: return K_PAGEUP;                  /* Channel up */
    case 0x31: return K_PAGEDOWN;                /* Channel down */
    case 0x71: return K_F1;                      /* Blue, Red, Green, Yellow */
    case 0x72: return K_F1 + 1;
    case 0x73: return K_F1 + 2;
    case 0x74: return K_F1 + 3;
    default:
        if (ui >= 0x21 && ui <= 0x29) return (uint16_t)(K_1 + (ui - 0x21)); /* Number 1..9 */
        return 0; /* diagonals, volume, media keys and the rest are not passed on */
    }
}

static unsigned up(struct cec_keys *k, struct cec_key_event *ev, unsigned n) {
    if (k->held) { ev[n].code = k->held; ev[n].value = 0; n++; k->held = 0; }
    return n;
}

unsigned cec_keys_pressed(struct cec_keys *k, uint8_t ui, uint32_t now_ms, struct cec_key_event *ev) {
    uint16_t code = cec_keys_map(ui);
    unsigned n = 0;
    if (!code) return 0;
    if (k->held && k->held == code && k->ui == ui) {
        ev[n].code = code; ev[n].value = 2; n++;          /* remote's own repeat */
    } else {
        n = up(k, ev, n);
        ev[n].code = code; ev[n].value = 1; n++;
        k->held = code; k->ui = ui;
    }
    k->last_ms = now_ms;
    return n;
}

unsigned cec_keys_released(struct cec_keys *k, struct cec_key_event *ev) { return up(k, ev, 0); }

unsigned cec_keys_tick(struct cec_keys *k, uint32_t now_ms, struct cec_key_event *ev) {
    if (k->held && (uint32_t)(now_ms - k->last_ms) >= CEC_KEY_RELEASE_MS) return up(k, ev, 0);
    return 0;
}
