/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). See include/looptrace.h. */
#include "looptrace.h"
struct looptrace looptrace0;
static const char *const names[LOOP_PHASES] = { "audio_null", "usb", "protocol", "card", "os", "cec", "hdmi_audio", "log_pump" };
static unsigned put(char *o, unsigned n, unsigned cap, const char *s) { while (*s && n + 1 < cap) o[n++] = *s++; return n; }
static unsigned dec(char *o, unsigned n, unsigned cap, uint32_t v) {
    char t[11]; unsigned k = 0;
    do { t[k++] = (char)('0' + v % 10u); v /= 10u; } while (v);
    while (k && n + 1 < cap) o[n++] = t[--k];
    return n;
}
unsigned looptrace_take(char *out, unsigned cap, int os_running) {
    unsigned n = 0, worst = 0;
    if (!cap) return 0;
    for (unsigned i = 1; i < LOOP_PHASES; i++) if (looptrace0.max_us[i] > looptrace0.max_us[worst]) worst = i;
    n = put(out, n, cap, "loop "); n = dec(out, n, cap, looptrace0.passes); n = put(out, n, cap, " passes, max ");
    n = dec(out, n, cap, looptrace0.max_us[worst]); n = put(out, n, cap, " us in "); n = put(out, n, cap, names[worst]);
    n = put(out, n, cap, os_running ? ", os running" : ", os not running"); out[n] = 0;
    looptrace0.passes = 0; for (unsigned i = 0; i < LOOP_PHASES; i++) looptrace0.max_us[i] = 0;
    return n;
}
