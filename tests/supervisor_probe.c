/* SPDX-License-Identifier: MIT; test-only observation of actual supervisor. */
#include "../core/os.c"
uint64_t probe_epoch(void) {return owner.active;}
uint64_t probe_last(void) {return owner.last;}
uint32_t probe_phase(void) {return owner.state;}
uint32_t probe_cursor(void) {return cleanup.cursor;}
void probe_last_set(uint64_t n) {owner.last=n;}
