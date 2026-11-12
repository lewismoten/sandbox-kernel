/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef AUDIO_ROT_H
#define AUDIO_ROT_H
#include <stdbool.h>
#include <stdint.h>
/* AUDIO.TXT keeps three boots: AUDIO.TXT (newest), AUDIO.1, AUDIO.2. Pure logic, host-tested;
   drivers/hdmi_audio.c supplies the card calls on the foreground path. */
struct audio_rot_io {int (*rename)(const char *from,const char *to,void *ctx);void *ctx;};
/* True when this boot's record ("n30,link,k0", see core/bootinfo.c) says the USB link kernel ran. */
bool audio_rot_is_link(const char *boot_record);
/* A link boot with no display attached has nothing to say about audio: it must not replace the
   record of the display boot before it. */
bool audio_rot_should_write(const char *boot_record,bool display_seen);
/* AUDIO.2 <- AUDIO.1 <- AUDIO.TXT. A missing older file is fine; returns the AUDIO.TXT rename result. */
int audio_rot_rotate(const struct audio_rot_io *io);
/* "AUDIO v2 boot=30 mode=link gen=7 dropped_lines=0" (no newline) into out; returns the length. */
unsigned audio_rot_header(char *out,unsigned cap,const char *boot_record,uint32_t generation,uint32_t dropped);
#endif
