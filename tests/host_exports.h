/* SPDX-License-Identifier: MIT; test-only entry points called by native/ctypes probes. */
#pragma once
#include <stdint.h>
#include <stdbool.h>

void advance(uint32_t us);
uint32_t write_count(void);
void inject_failure(uint32_t after);
void open_disk(const char *path);
void close_disk(void);
void set_media_sectors(uint32_t sectors);
uint32_t read_count(void);
void probe_run_on_read(void);
int probe_run_result(void);
void fail_read_sector(uint32_t lba);
void protocol_on_next_sd_read(void);
const char *take_output(void);
void feed(const void *data,unsigned n);
bool initialize(void);
uint32_t *kernel_table(void);
const char *take_log(void);
uint8_t *fb_setup(uint32_t order,int present);
void probe_stop_on_mode(void);

uint64_t probe_epoch(void);
uint64_t probe_last(void);
uint32_t probe_phase(void);
uint32_t probe_cursor(void);
void probe_last_set(uint64_t n);
void probe_corrupt_after_prepare(void);
void probe_late_copy_failure(void);
void probe_stop_after_prepare(void);
uint32_t probe_l2_count(void);
void probe_fail_map(unsigned n);

void panel_probe_fault(unsigned kind);
bool panel_probe_disabled(void);
unsigned panel_probe_full_count(void);
unsigned panel_probe_partial_count(void);
uint64_t panel_probe_boot_trigger(void);
uint64_t panel_probe_last_full_trigger(void);
uint64_t panel_probe_last_trigger(void);
