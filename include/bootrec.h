/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef BOOTREC_H
#define BOOTREC_H
#include "bootinfo.h"
/* Kernel side of the boot record (core/bootinfo.c has the text rules): needs a mounted card. */
void bootrec_boot(enum boot_decision d,bool new_boot); /* read /OS/NAME.TXT and the last record; write this boot's (counter +1 when new_boot) */
void bootrec_keyboards(unsigned count);                /* host role: rewrite this boot's record with the keyboards found */
#endif
