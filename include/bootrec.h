/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef BOOTREC_H
#define BOOTREC_H
#include "bootinfo.h"
/* Kernel side of the boot record (core/bootinfo.c has the text rules): needs a mounted card. */
void bootrec_boot(enum boot_decision d,bool new_boot); /* read /OS/NAME.TXT and the last record; write this boot's (counter +1 when new_boot) */
void bootrec_keyboards(unsigned count);                /* host role: rewrite this boot's record with the keyboards found */
void bootrec_usblog(uint32_t attach_events,uint32_t remove_events); /* host role: save the USB log (core/usblog.c) to /OS/SAVES/KERNEL/USB.TXT */
/* Device role, for USBLOG: part i (0-based) of USB.TXT in out, newlines as '|', at most USBLOG_PART bytes.
   Returns the number of parts in the file (0 = no file), or -1 if unreadable. */
#define USBLOG_PART 280
int bootrec_usblog_part(unsigned i,char *out);
#endif
