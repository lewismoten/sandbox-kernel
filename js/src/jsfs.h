/* SPDX-License-Identifier: MIT; the app's file API (slice 4a): file handles with binary reads and seek, stat, folder
 * listings with types and sizes, and the staged writer. jsrt.c registers it and supplies the capability and path checks. */
#pragma once
#include "quickjs.h"
#include <stdint.h>
/* The most one app may hold in its save folder (and in one staged file); the kernel's 1 MiB single write is the hard cap. */
#define JSFS_KERNEL_MAX (1u << 20)
extern uint32_t jsrt_write_quota; /* default 256 KiB */
/* Registers the classes and native functions on `native` (once per runtime and context). */
void jsfs_init(JSRuntime *rt, JSContext *ctx, JSValueConst native);
/* From jsrt.c: capability and path policy. need: 0, or -1 with the error thrown. check: 0 if allowed, else -1 and *why. */
int jsrt_need(JSContext *c, uint32_t any_cap, const char *what);
int jsrt_path_check(const char *path, int write, const char **why);
int jsrt_copy_str(JSContext *c, JSValueConst v, char *out, size_t cap);
uint8_t *jsrt_io(size_t *size); /* the shared 256 KiB read buffer */
