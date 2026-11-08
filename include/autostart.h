/* SPDX-License-Identifier: MIT; which OS program the kernel starts 10 s after
 * boot, and the one fallback to the diagnostics program when it keeps
 * faulting (docs/m1.md, "OS autostart"). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define AUTOSTART_DEFAULT "/OS/GMOS.ELF"
#define AUTOSTART_FALLBACK "/OS/DIAG.ELF"
#define AUTOSTART_CHOICE "/OS/AUTORUN.TXT"
/* The first line of AUTORUN.TXT when it names a program in /OS: 1 to 8 of
   A-Z 0-9 _ - then ".ELF" (case folded); out gets "/OS/<NAME>". */
bool autostart_name(const char *text,uint32_t bytes,char out[20]);
/* The program to start: AUTORUN.TXT's choice if valid and present, else
   GMOS.ELF if present, else DIAG.ELF. Logs where the choice came from. */
const char *autostart_pick(void);
/* Start the picked program (DIAG.ELF if it is rejected); 0 or a negative
   errno from the last attempt. Arms the fault fallback. */
int autostart_run(const char *path);
/* USB RUN/STOP takes over: no boot launch or fault fallback afterward. */
void autostart_cancel(void);
bool autostart_cancelled(void);
/* From the supervisor once the restart policy gives up: true if DIAG.ELF
   was started in place of the autostarted program. */
bool autostart_fallback(void);
