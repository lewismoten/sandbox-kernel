/* SPDX-License-Identifier: MIT; AUDIO.TXT format and bounded whole-line eviction. */
#include <assert.h>
#include <string.h>
#include "audio_diag.h"
int main(void) {
 struct audio_diag l = {0};
 struct audio_observation o = {.attempt = 1, .pixel_id = 9, .rate = 48000, .mailbox = "MBOX TAG"};
 audio_diag_decision(&l, 123, &o, 0, "firmware pixel clock unavailable");
 assert(strstr(l.text, "123 backend=null reason=firmware pixel clock unavailable attempt=1\n"));
 assert(strstr(l.text, "pixel_tag=0x00030002 clock_id=9 ok=0 reply_id=9 pixel_hz=0 mailbox=MBOX TAG\n"));
 assert(strstr(l.text, "channel_status_left=000000000000000000000000000000000000000000000000\n"));
 assert(strstr(l.text, "mode=dvi\n") && l.dirty && !l.dropped);
 o.pixel_ok = 1; o.pixel_hz = 148500000; o.scheduler = 3; o.sample_hz = 48000;
 o.mailbox = "ok"; o.attempt = 2; o.left[0] = 0x12; o.right[23] = 0x34;
 audio_diag_decision(&l, 456, &o, 1, "ok");
 assert(strstr(l.text, "456 backend=hdmi reason=ok attempt=2\n"));
 assert(strstr(l.text, "pixel_hz=148500000 mailbox=ok\n"));
 assert(strstr(l.text, "mode=hdmi\n"));
 assert(strstr(l.text, "sample_hz=48000 channels=2"));
 assert(strstr(l.text, "channel_status_left=120000000000000000000000000000000000000000000000\n"));
 assert(strstr(l.text, "channel_status_right=000000000000000000000000000000000000000000000034\n"));
 uint8_t edid[128] = {2, 3, 8, 0x40, 0x23, 0x09, 0x07, 0x07};
 audio_diag_edid(&l, 500, 1, edid);
 assert(strstr(l.text, "500 edid_block=1 cta_basic_audio=1\n"));
 assert(strstr(l.text, "500 edid_block=1 audio_data_block=090707\n"));
 edid[4] = 0x3f; audio_diag_edid(&l, 600, 1, edid); /* collection overrun */
 assert(strstr(l.text, "600 edid_audio=malformed\n"));
 edid[2] = 0; audio_diag_edid(&l, 700, 1, edid);
 assert(strstr(l.text, "700 edid_audio=no-audio-data-block\n"));
 audio_diag_line(&l, 800, "bad\nrecord\001"); assert(strstr(l.text, "800 bad?record?\n"));
 char big[1024]; memset(big, 'x', sizeof big); big[1023] = 0;
 audio_diag_line(&l, UINT32_MAX, big);
 for (unsigned i = 0; i < 1000; i++) audio_diag_line(&l, 900, "whole line");
 assert(l.len <= AUDIO_LOG_BYTES && strlen(l.text) == l.len && l.dropped);
 assert(!strncmp(l.text, "900 whole line\n", 15));
 assert(l.text[l.len - 1] == '\n');
 /* Maximal fixed fields and caller text still fit their formatting buffers. */
 memset(&o, 0xff, sizeof o); o.mailbox = big;
 audio_diag_decision(&l, UINT32_MAX, &o, 1, big);
 assert(l.len <= AUDIO_LOG_BYTES && l.text[l.len] == 0);
 return 0;
}
