/* SPDX-License-Identifier: MIT; the core profile: no game. <game> is an inert element
 * (it can be created and sized; play() throws a TypeError and nothing ever draws),
 * and a game folder as an <img src> is not shown. */
#include "profile.h"
static const struct jsrt_profile core = {JSRT_PROFILE_ABI, "core", 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
const struct jsrt_profile *const jsrt_profile = &core;
