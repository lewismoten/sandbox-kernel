/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten */
#ifndef BOOTINFO_H
#define BOOTINFO_H
#include <stdbool.h>
#include <stdint.h>
/* What this boot decided about the USB port, as one plain record, and the Pi's short name. Pure
   text logic (host-tested); core/bootrec.c reads and writes the card. */
enum boot_decision { BD_NONE, BD_LINK, BD_NO_ADAPTER, BD_INSTALL_KBD, BD_KEYBOARD };
#define BOOTINFO_REC_MAX 40
#define BOOTINFO_NAME_MAX 8
/* E-ink status text for a decision (16 columns), or NULL when the normal state says it. */
const char *bootinfo_status(enum boot_decision d);
const char *bootinfo_decision_name(enum boot_decision d); /* "link", "no-adapter", "install-kbd", "keyboard", "none" */
/* "n30,keyboard,k2" into out (BOOTINFO_REC_MAX bytes); returns the length. */
unsigned bootinfo_format(char *out,uint32_t n,enum boot_decision d,unsigned keyboards);
/* The boot counter at the start of a record, or false. Stops at the first control character. */
bool bootinfo_parse_count(const char *text,uint32_t bytes,uint32_t *n);
/* NAME.TXT: first line, 1 to 8 of A-Z 0-9 -; anything else is ignored (false, out empty). */
bool bootinfo_parse_name(const char *text,uint32_t bytes,char out[BOOTINFO_NAME_MAX+1]);
/* Remembered text for INFO: the previous boot's record, this boot's record, the name ("" = none). */
void bootinfo_set_prev(const char *text,uint32_t bytes);
void bootinfo_set_this(const char *text);
void bootinfo_set_name(const char *name);
const char *bootinfo_prev(void);
const char *bootinfo_this(void);
const char *bootinfo_name(void);
#endif
