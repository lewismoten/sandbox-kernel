/* SPDX-License-Identifier: MIT; compile-only native/ARM layout witness. */
#include "app_arena_core.h"
#include "fb.h"
#include <stddef.h>
_Static_assert(AA_FB_BYTES==FB_SURFACE_MAX,"existing surface reservation");
_Static_assert(offsetof(struct aa_owner,last)==0,"last");
_Static_assert(offsetof(struct aa_owner,active)==8,"active");
_Static_assert(offsetof(struct aa_owner,pa)==16,"pa");
_Static_assert(offsetof(struct aa_owner,bytes)==20,"bytes");
_Static_assert(offsetof(struct aa_owner,state)==24,"state");
_Static_assert(offsetof(struct aa_owner,reserved)==28,"reserved");
_Static_assert(offsetof(struct aa_cleanup,restart)==16,"restart");
_Static_assert(offsetof(struct aa_cleanup,decision_time)==48,"timestamp");
_Static_assert(offsetof(struct aa_cleanup,exit_status)==56,"exit");
_Static_assert(sizeof(struct aa_space)==(sizeof(void*)==4?36:56),"transient view");
const uint32_t aa_layout_probe[]={sizeof(struct aa_owner),sizeof(struct aa_checkpoint),
 sizeof(struct aa_cleanup),sizeof(struct aa_space),AA_BASE,AA_BYTES,AA_FLOOR,
 offsetof(struct aa_owner,active),offsetof(struct aa_cleanup,decision_time)};
