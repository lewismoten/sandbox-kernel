/* SPDX-License-Identifier: MIT; the two memory routines a freestanding
   compiler may call on its own (structure copies and zeroing). */
#include <stddef.h>
void *memset(void *d, int c, size_t n) {
  unsigned char *p = d;
  while (n--) *p++ = (unsigned char)c;
  return d;
}
void *memcpy(void *d, const void *s, size_t n) {
  unsigned char *p = d;
  const unsigned char *q = s;
  while (n--) *p++ = *q++;
  return d;
}
