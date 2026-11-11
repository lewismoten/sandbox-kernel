/* SPDX-License-Identifier: MIT; foreground OS idle policy, no hardware dependencies. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#define POWER_NO_BLANK 1u
#define POWER_NO_SLOWDOWN 2u
#define POWER_HOLDS 16u
#define POWER_POLL_US 1000u
struct power_state {
 uint64_t last_input, next_token;
 uint64_t tokens[POWER_HOLDS];
 uint32_t flags[POWER_HOLDS], slow_seconds, blank_seconds;
};
extern struct power_state power0;
void power_reset(uint64_t now); /* revoke all holds and restore defaults; preserve token epoch */
void power_input(uint64_t now);
int power_acquire(uint64_t flags,uint64_t *token);
int power_release(uint64_t token);
int power_timeouts(uint64_t slow,uint64_t blank,uint64_t now);
uint32_t power_target(uint64_t now); /* NO_* bits name resources to put to sleep */
uint32_t power_wait_us(uint64_t now,uint64_t deadline);
void power_hw_init(void);
void power_hw_task(void);
void idle_wait(uint32_t us);
void cec_hw_power(bool standby);
