/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). Bounded CEC session log. */
#include "cec_log.h"

void cec_log_init(struct cec_log *l) { l->len = 0; l->head_end = 0; l->dropped = 0; l->lines = 0; l->dirty = 0; l->text[0] = 0; }

static uint32_t put_num(char *o, uint32_t n) {
    char t[11]; unsigned k = 0; uint32_t w = 0;
    do { t[k++] = (char)('0' + n % 10u); n /= 10u; } while (n);
    while (k) o[w++] = t[--k];
    return w;
}

void cec_log_line(struct cec_log *l, uint32_t now_ms, const char *text) {
    char line[CEC_LOG_LINE + 12]; uint32_t n = put_num(line, now_ms);
    line[n++] = ' ';
    for (unsigned i = 0; text[i] && i < CEC_LOG_LINE - 1u; i++)
        line[n++] = (text[i] >= 32 && text[i] < 127) ? text[i] : '?';
    line[n++] = '\n';
    while (l->len + n > CEC_LOG_BYTES) {          /* drop the oldest whole line */
        uint32_t f = l->head_end, k = f;
        while (k < l->len && l->text[k] != '\n') k++;
        k = k < l->len ? k + 1 : l->len;
        for (uint32_t i = k; i < l->len; i++) l->text[i - (k - f)] = l->text[i];
        l->len -= k - f; l->dropped++; if (l->lines) l->lines--;
    }
    for (uint32_t i = 0; i < n; i++) l->text[l->len + i] = line[i];
    l->len += n; l->lines++; if (!l->head_end && l->len >= CEC_LOG_HEAD) l->head_end = l->len; l->text[l->len] = 0; l->dirty = 1;
}

uint32_t cec_log_tail(const struct cec_log *l, unsigned n, char *out, uint32_t cap) {
    uint32_t start = l->len; unsigned seen = 0;
    while (start > 0) {                           /* walk back n newlines */
        if (l->text[start - 1] == '\n' && start != l->len) { if (++seen == n) break; }
        start--;
    }
    uint32_t w = 0;
    for (uint32_t i = start; i < l->len && w + 1 < cap; i++) out[w++] = l->text[i];
    if (cap) out[w] = 0;
    return w;
}
