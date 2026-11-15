/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef CEC_ROT_H
#define CEC_ROT_H
#include <stdbool.h>
#include <stdint.h>
/* CEC.TXT keeps three boots, the way AUDIO.TXT does (core/audio_rot.c): CEC.TXT (newest), CEC.1, CEC.2.
   The rotation runs before a boot's first CEC.TXT write, one rename per call so that no main-loop pass does two
   24 KiB copy-on-write renames. Pure logic, host-tested; drivers/cec_hw.c supplies the card calls. */
struct cec_rot_io {
 int (*rename)(const char *from,const char *to,void *ctx);
 bool (*has_text)(const char *name,void *ctx); /* the file exists and its first byte is not NUL (not empty padding) */
 void *ctx;
};
/* One step: *step starts at 0. Returns 1 when another call is needed, 0 when the rotation is finished (including
   "nothing to rotate": CEC.TXT absent or empty leaves CEC.1 and CEC.2 alone). A failed rename is ignored. */
int cec_rot_step(const struct cec_rot_io *io,unsigned *step);
#endif
