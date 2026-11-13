/* SPDX-License-Identifier: MIT; the host fault build's allocator (src/alloc.h): counts every allocation in two series (the C
 * bindings' own, "direct", and QuickJS's through jsrt.c's heap wrapper, "qjs"), counts what is still live, and fails the
 * fi_dfail-th or fi_qfail-th one (1-based; 0 = none). tests/host/jsrun.c --sweep drives it. */
#include <stdlib.h>
void *fi_malloc(size_t n); void *fi_calloc(size_t a, size_t b); void *fi_realloc(void *p, size_t n); void fi_free(void *p);
void *fi_qmalloc(size_t n); void *fi_qcalloc(size_t a, size_t b); void *fi_qrealloc(void *p, size_t n); void fi_qfree(void *p);
unsigned long fi_dcount, fi_qcount, fi_dfail, fi_qfail, fi_hits;
long fi_dlive, fi_qlive;
static int fail(unsigned long *count, unsigned long at) {++*count; if (at && *count == at) {fi_hits++; return 1;} return 0;}
void *fi_malloc(size_t n) {if (fail(&fi_dcount, fi_dfail)) return 0; void *p = malloc(n); if (p) fi_dlive++; return p;}
void *fi_calloc(size_t a, size_t b) {if (fail(&fi_dcount, fi_dfail)) return 0; void *p = calloc(a, b); if (p) fi_dlive++; return p;}
void *fi_realloc(void *p, size_t n) {
  if (fail(&fi_dcount, fi_dfail)) return 0;
  void *q = realloc(p, n); if (q && !p) fi_dlive++; return q;
}
void fi_free(void *p) {if (p) fi_dlive--; free(p);}
void *fi_qmalloc(size_t n) {if (fail(&fi_qcount, fi_qfail)) return 0; void *p = malloc(n); if (p) fi_qlive++; return p;}
void *fi_qcalloc(size_t a, size_t b) {if (fail(&fi_qcount, fi_qfail)) return 0; void *p = calloc(a, b); if (p) fi_qlive++; return p;}
void *fi_qrealloc(void *p, size_t n) {
  if (fail(&fi_qcount, fi_qfail)) return 0;
  void *q = realloc(p, n); if (q && !p) fi_qlive++; return q;
}
void fi_qfree(void *p) {if (p) fi_qlive--; free(p);}
