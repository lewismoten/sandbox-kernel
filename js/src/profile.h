/* SPDX-License-Identifier: MIT; the profile interface: what the core (jsrt.c) calls
 * to let an optional profile (the Game-Maker play core, or a test) give the <game>
 * element and the picture loader their content. The core owns the order of init,
 * step and stop and never assumes a profile; every hook may be NULL. One profile
 * is chosen at build time (JS_PROFILE in the Makefile) by defining jsrt_profile.
 * Hooks run on the single interpreter thread and must not call back into jsrt_*.
 * docs/profiles.md is the author's guide. */
#pragma once
#include <stdint.h>
#define JSRT_PROFILE_ABI 1
#define JSRT_PIC_W 320u /* the direct picture's size (the play core's), also what screen.gameLowres() checks against */
#define JSRT_PIC_H 200u
/* step() and key() results (or'ed). */
#define JSRT_DRAW 1u /* a new picture: draw it (direct) or repaint the element */
#define JSRT_END 2u  /* the element is finished: the core stops it ("ended" fires) */
/* step() fills this when it returns JSRT_DRAW and the screen is in the direct 8 bpp
   mode: JSRT_PIC_W x JSRT_PIC_H indices and a 768-byte 6-bit palette, valid until the next call. */
struct jsrt_direct {const uint8_t *px; const uint8_t *pal;};
struct jsrt_profile {
  uint32_t abi;                       /* JSRT_PROFILE_ABI */
  const char *name;                   /* for logs and tests */
  /* Order, per app: init; (play, then step/key/wake each turn, then end); stop. The pair
     init/stop is made for every jsrt_start, even a failed one, and stop runs once. */
  void (*init)(void);                 /* after the core's state is reset, before the JS context exists */
  void (*stop)(void);                 /* last, after end if the element was playing, before the context is freed */
  /* The <game> element. NULL play: the element is inert (play() throws a TypeError). */
  int (*play)(const char *dir, void **handle, char *err, uint32_t cap); /* 0, or -1 with a message in err; handle is what draw gets */
  void (*end)(void);                  /* the element stopped (any reason): release what play took */
  int (*key)(uint16_t code, int32_t value);   /* while the element has focus (or game.press); returns JSRT_END or 0 */
  uint32_t (*step)(uint64_t now, struct jsrt_direct *d); /* each turn while playing; returns JSRT_DRAW / JSRT_END flags */
  uint64_t (*wake)(uint64_t now);     /* when the element next needs a turn (ns), 0 for no need */
  void (*draw)(void *handle, uint32_t *fb, uint32_t stride); /* paint the element over its rectangle (32 bpp page) */
  uint32_t (*ticks)(void);            /* game.ticks */
  /* Pictures: a src the core cannot read itself (not a .gif), e.g. a game folder's title screen.
     Fill JSRT_PIC_W x JSRT_PIC_H indices and a 768-byte palette; 0 or -1. */
  int (*picture)(const char *src, uint8_t *px, uint8_t *pal);
};
/* Defined by exactly one profile file in the build (profile_core.c by default). */
extern const struct jsrt_profile *const jsrt_profile;
/* For profiles: one line in the platform log. */
void jsrt_log(const char *s);
