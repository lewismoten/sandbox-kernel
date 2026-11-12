/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). A bounded text log of one CEC session (CEC.TXT). When it
 * is full the oldest whole lines after the first CEC_LOG_HEAD bytes are dropped (the start of the
 * session, with its init and session lines, is always kept); a counter says how many. */
#ifndef CEC_LOG_H
#define CEC_LOG_H
#include <stdint.h>
#define CEC_LOG_BYTES 24576u
#define CEC_LOG_HEAD 6144u
#define CEC_LOG_LINE 128u
struct cec_log { char text[CEC_LOG_BYTES + 1u]; uint32_t len, head_end, dropped, lines; uint8_t dirty; };
void cec_log_init(struct cec_log *l);
/* Appends "<ms> <text>\n" (text cut to CEC_LOG_LINE-1 characters, control characters shown as ?). */
void cec_log_line(struct cec_log *l, uint32_t now_ms, const char *text);
/* Up to n lines from the end, copied into out (NUL terminated, newline per line) for USB replay. */
uint32_t cec_log_tail(const struct cec_log *l, unsigned n, char *out, uint32_t cap);
#endif
