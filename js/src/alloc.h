/* SPDX-License-Identifier: MIT; every allocation in the runtime goes through here. On the board (and in the normal host
 * build) these are plain malloc, calloc, realloc and free. The host fault build (JSRT_FAULT, tests/host/jsrun.c) counts
 * them and can fail the N-th one, in two series: the C bindings' own (direct) and QuickJS's (through the heap wrapper in
 * jsrt.c), so tests/alloc_run.py can fail every allocation a scripted app reaches. Include it after <stdlib.h>. */
#pragma once
#include <stdlib.h>
#ifdef JSRT_FAULT
void *fi_malloc(size_t n);
void *fi_calloc(size_t a, size_t b);
void *fi_realloc(void *p, size_t n);
void fi_free(void *p);
void *fi_qmalloc(size_t n);
void *fi_qcalloc(size_t a, size_t b);
void *fi_qrealloc(void *p, size_t n);
void fi_qfree(void *p);
#define malloc fi_malloc
#define calloc fi_calloc
#define realloc fi_realloc
#define free fi_free
#define JSRT_QMALLOC fi_qmalloc
#define JSRT_QCALLOC fi_qcalloc
#define JSRT_QREALLOC fi_qrealloc
#define JSRT_QFREE fi_qfree
#else
#define JSRT_QMALLOC malloc
#define JSRT_QCALLOC calloc
#define JSRT_QREALLOC realloc
#define JSRT_QFREE free
#endif
