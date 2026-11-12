/* SPDX-License-Identifier: MIT; bounded, host-testable AUDIO.TXT records. */
#pragma once
#include <stdint.h>
#define AUDIO_LOG_BYTES 8192u
#define AUDIO_LOG_LINE 192u
struct audio_diag {
 char text[AUDIO_LOG_BYTES + 1];
 uint32_t len, dropped;
 int dirty;
};
struct audio_observation {
 uint32_t attempt, pixel_ok, pixel_id, pixel_hz, pixel_used, mode_w, mode_h;
 const char *pixel_src; /* "", "firmware-measured" or "mode-table" */
 uint32_t hsm_ctl, hsm_div, m_ctl, scheduler, hotplug, rate, sample_hz;
 uint8_t left[24], right[24];
 const char *mailbox;
};
void audio_diag_line(struct audio_diag *log, uint32_t ms, const char *line);
/* Like audio_diag_line for a periodic "stat ..." line: drops older stat lines before others. */
void audio_diag_stat(struct audio_diag *log, uint32_t ms, const char *line);
void audio_diag_decision(struct audio_diag *log, uint32_t ms,
                         const struct audio_observation *o, int active, const char *reason);
/* EDID blocks are forwarded by CEC after mailbox/status/checksum validation.
 * Records CTA basic-audio and raw Audio Data Blocks, never infers support. */
void audio_diag_edid(struct audio_diag *log, uint32_t ms, unsigned block, const uint8_t *data);
