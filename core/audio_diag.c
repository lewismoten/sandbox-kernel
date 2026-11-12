/* SPDX-License-Identifier: MIT; AUDIO.TXT format, no hardware or libc formatting. */
#include "audio_diag.h"

static void text(char **p, const char *s) { while (*s) *(*p)++ = *s++; **p = 0; }
static void dec(char **p, uint32_t v) {
 char t[10]; unsigned n = 0;
 do { t[n++] = (char)('0' + v % 10); v /= 10; } while (v);
 while (n) *(*p)++ = t[--n];
 **p = 0;
}
static void field(char **p, const char *key, uint32_t v) { text(p, key); dec(p, v); }
static void hex(char **p, const uint8_t *b, unsigned n) {
 for (unsigned i = 0; i < n; i++) { *(*p)++ = "0123456789abcdef"[b[i] >> 4]; *(*p)++ = "0123456789abcdef"[b[i] & 15]; }
 **p = 0;
}
void audio_diag_line(struct audio_diag *l, uint32_t ms, const char *s) {
 char line[AUDIO_LOG_LINE + 12], *p = line;
 dec(&p, ms); text(&p, " ");
 for (unsigned i = 0; s[i] && i < AUDIO_LOG_LINE - 1; i++)
  *p++ = s[i] >= 32 && s[i] < 127 ? s[i] : '?';
 *p++ = '\n'; uint32_t n = (uint32_t)(p - line);
 while (l->len + n > AUDIO_LOG_BYTES) {
  uint32_t k = 0; while (k < l->len && l->text[k++] != '\n') {}
  for (uint32_t i = k; i < l->len; i++) l->text[i - k] = l->text[i];
  l->len -= k; l->dropped++;
 }
 for (uint32_t i = 0; i < n; i++) l->text[l->len + i] = line[i];
 l->len += n; l->text[l->len] = 0; l->dirty = 1;
}
static int is_stat(const char *t) { return t[0]=='s' && t[1]=='t' && t[2]=='a' && t[3]=='t' && t[4]==' '; }
/* A periodic "stat " line: when the log is full, retire the oldest stat lines
 * first so the boot and decision lines at the top survive a long session. */
void audio_diag_stat(struct audio_diag *l, uint32_t ms, const char *s) {
 uint32_t need = 12; while (s[need - 12] && need < AUDIO_LOG_LINE + 12) need++;
 while (l->len + need > AUDIO_LOG_BYTES) {
  uint32_t a = 0, found = 0;
  while (a < l->len && !found) {
   uint32_t b = a; while (b < l->len && l->text[b] != '\n') b++;
   uint32_t d = a; while (d < b && l->text[d] >= '0' && l->text[d] <= '9') d++;
   if (d < b && l->text[d] == ' ' && b - d > 5 && is_stat(l->text + d + 1)) {
    b++; for (uint32_t i = b; i < l->len; i++) l->text[i - (b - a)] = l->text[i];
    l->len -= b - a; found = 1;
   } else a = b + 1;
  }
  if (!found) break;
 }
 audio_diag_line(l, ms, s);
}
void audio_diag_decision(struct audio_diag *l, uint32_t ms,
                         const struct audio_observation *o, int active, const char *reason) {
 char b[AUDIO_LOG_LINE], *p = b;
 text(&p, active ? "backend=hdmi reason=" : "backend=null reason=");
 /* Bound caller-supplied reasons, including unexpected firmware errors. */
 for (unsigned i = 0; reason[i] && i < 110; i++) *p++ = reason[i];
 *p = 0; field(&p, " attempt=", o->attempt); audio_diag_line(l, ms, b);
 p = b; field(&p, "pixel_tag=0x00030002 clock_id=9 ok=", o->pixel_ok);
 field(&p, " reply_id=", o->pixel_id); field(&p, " pixel_hz=", o->pixel_hz);
 text(&p, " mailbox=");
 for (unsigned i = 0; o->mailbox[i] && i < 32; i++) *p++ = o->mailbox[i];
 *p = 0; audio_diag_line(l, ms, b);
 p = b; text(&p, "pixel_source=");
 text(&p, o->pixel_src && *o->pixel_src ? o->pixel_src : "firmware-clock");
 field(&p, " pixel_used_hz=", o->pixel_used ? o->pixel_used : o->pixel_hz);
 field(&p, " mode_w=", o->mode_w); field(&p, " mode_h=", o->mode_h);
 *p = 0; audio_diag_line(l, ms, b);
 p = b; field(&p, "hsm_ctl=", o->hsm_ctl); field(&p, " hsm_div=", o->hsm_div);
 field(&p, " hd_m_ctl=", o->m_ctl); field(&p, " scheduler=", o->scheduler);
 field(&p, " hotplug=", o->hotplug); text(&p, (o->scheduler & 2) ? " mode=hdmi" : " mode=dvi");
 audio_diag_line(l, ms, b);
 p = b; field(&p, "requested_rate=", o->rate); field(&p, " sample_hz=", o->sample_hz);
 text(&p, " channels=2 channel_status_source=kernel-iec958"); audio_diag_line(l, ms, b);
 p = b; text(&p, "channel_status_left="); hex(&p, o->left, 24); audio_diag_line(l, ms, b);
 p = b; text(&p, "channel_status_right="); hex(&p, o->right, 24); audio_diag_line(l, ms, b);
}
void audio_diag_edid(struct audio_diag *l, uint32_t ms, unsigned block, const uint8_t *b) {
 char line[AUDIO_LOG_LINE], *p = line;
 field(&p, "edid_block=", block);
 if (!block || b[0] != 2) { text(&p, " audio=not-cta"); audio_diag_line(l, ms, line); return; }
 field(&p, " cta_basic_audio=", (b[3] >> 6) & 1); audio_diag_line(l, ms, line);
 unsigned end = b[2], found = 0;
 if (end == 0) end = 4; /* no data block collection */
 if (end < 4 || end > 127) { audio_diag_line(l, ms, "edid_audio=malformed"); return; }
 for (unsigned i = 4; i < end;) {
  unsigned tag = b[i] >> 5, n = b[i] & 31;
  if (i + 1 + n > end) { audio_diag_line(l, ms, "edid_audio=malformed"); return; }
  if (tag == 1) {
   p = line; field(&p, "edid_block=", block); text(&p, " audio_data_block=");
   hex(&p, b + i + 1, n); audio_diag_line(l, ms, line); found++;
  }
  i += 1 + n;
 }
 if (!found) audio_diag_line(l, ms, "edid_audio=no-audio-data-block");
}
