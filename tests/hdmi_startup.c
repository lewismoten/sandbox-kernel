/* SPDX-License-Identifier: MIT; production HDMI startup with bounded fake MMIO. */
#include <assert.h>
#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "audio_null.h"
#include "hdmi_audio.h"
#include "iec958.h"
#include "mixer.h"
#include "audio_diag.h"

#define PERIPH 0x20000000u
#define REG(address) (*mock_register(address))
static uint32_t *mock_register(uint32_t address);
static uint32_t bus(const void *pointer);
static void drain(void);
static void hd(uint32_t offset, uint32_t value);
static void hc(uint32_t offset, uint32_t value);
static uint32_t dma(uint32_t offset);
static void dmaw(uint32_t offset, uint32_t value);
static void barrier(void) {}
static uint32_t irq_save(void) { return 0; }
static void irq_restore(uint32_t flags) { (void)flags; }
static uint32_t micros(void);
static int expired(uint32_t start, uint32_t duration);
static void delay_us(uint32_t duration);
static int property(uint32_t tag, uint32_t *values, unsigned count);
const char *property_fail = "MBOX TAG";
bool storage_ready = true;
static bool card_busy;
static char saved[AUDIO_LOG_BYTES + 80];
static unsigned card_writes, pixel_queries;
static bool os_on;
static bool protocol_busy(void) { return card_busy; }
bool os_running(void) { return os_on; }
static void log_line(const char *line) { (void)line; }
static void boot_note(const char *line) { (void)line; }
static void number(char *out, uint32_t value) { snprintf(out, 12, "%u", value); }

#include "hdmi-driver.h"

uint32_t boot_generation = 7;
const char *bootinfo_this(void) { return "n7,keyboard,k0"; }
static unsigned renames;
int fatsave_rename(const char *g, const char *f, const char *t, uint8_t *s, uint32_t c) {
 (void)g; (void)f; (void)t; (void)s; (void)c; renames++; return 0;
}
int fatsave_put(const char *game, const char *name, const uint8_t *data, uint32_t bytes) {
 assert(!strcmp(game, "KERNEL") && !strcmp(name, "AUDIO.TXT"));
 assert(bytes < sizeof saved); memcpy(saved, data, bytes); saved[bytes] = 0;
 card_writes++; return 0;
}
static uint32_t hd_regs[64], core_regs[384], misc_regs[5], dma_regs[9];
static struct { uint32_t address, value; } writes[128];
static unsigned write_count, scenario, packet_stuck;
static uint32_t now, pixel_hz, pixel_reply_id;
static int pixel_query_ok;
uint64_t clock_us(void) { return now; }

static uint32_t *mock_register(uint32_t address) {
 if (address >= HDMI_HD_BASE && address < HDMI_HD_BASE + sizeof hd_regs)
  return &hd_regs[(address - HDMI_HD_BASE) / 4];
 if (address >= HDMI_CORE_BASE && address < HDMI_CORE_BASE + sizeof core_regs)
  return &core_regs[(address - HDMI_CORE_BASE) / 4];
 const uint32_t addresses[] = {CM_HSMCTL, CM_HSMDIV, DMA_ENABLE, PERIPH + 0xb210u, PERIPH + 0xb21cu};
 for (unsigned index = 0; index < 5; index++) if (address == addresses[index]) return &misc_regs[index];
 assert(!"unexpected MMIO address"); return NULL;
}
static void record(uint32_t address, uint32_t value) {
 assert(write_count < 128);
 writes[write_count].address = address; writes[write_count++].value = value;
}
static uint32_t bus(const void *pointer) {
 assert(pointer == cbs || pointer == buf);
 return pointer == cbs ? 0xc0100000u : 0xc0200000u;
}
static void drain(void) {}
static void hd(uint32_t offset, uint32_t value) {
 record(HDMI_HD_BASE + offset, value);
 REG(HDMI_HD_BASE + offset) = value;
}
static void hc(uint32_t offset, uint32_t value) {
 record(HDMI_CORE_BASE + offset, value);
 REG(HDMI_CORE_BASE + offset) = value;
 if (offset == HC_RAM_PACKET_CONFIG && !packet_stuck)
  core_regs[HC_RAM_PACKET_STATUS / 4] = (value & 0x10000u) ? value & 0x1fu : 0;
}
static uint32_t dma(uint32_t offset) { assert(offset <= 0x20u); return dma_regs[offset / 4]; }
static void dmaw(uint32_t offset, uint32_t value) {
 record(DMA_BASE + (uint32_t)hw.ch * 0x100u + offset, value);
 if (offset == DMA_CS && value & 0x80000000u) {
  dma_regs[DMA_CS / 4] = scenario == 4 ? value : 0;
  dma_regs[DMA_CONBLK / 4] = 0;
 } else if (offset == DMA_CS && value == 6u) dma_regs[0] &= ~6u;
 else dma_regs[offset / 4] = value;
}
static uint32_t micros(void) { now += 1000; return now; }
static int expired(uint32_t start, uint32_t duration) { return micros() - start >= duration; }
static void delay_us(uint32_t duration) {
 assert(duration == 30000);
 assert(dma_regs[0] == (1u | (1u << 28) | (8u << 16) | (15u << 20)));
 assert(misc_regs[3] == (1u << (16u + (uint32_t)hw.ch)));
 if (scenario != 1) dma_regs[1] += 3 * 32;
 if (!scenario) completions = 3;
 if (scenario == 3) dma_regs[0] |= 0x100u;
 if (scenario == 5) dma_regs[0] |= 4u;
 now += duration;
}
static uint32_t hsm_fw_hz, measured_hz, mode_w, mode_h;
static int property(uint32_t tag, uint32_t *values, unsigned count) {
 if (tag == 0x60001) { assert(count == 1); values[0] = 1u << 2; return 1; }
 if ((tag == 0x30047 || tag == 0x30002) && values[0] == 13) { values[1] = hsm_fw_hz; return hsm_fw_hz != 0; }
 if (tag == 0x30047) { values[0] = 9; values[1] = measured_hz; return measured_hz != 0; }
 if (tag == 0x40003) { values[0] = mode_w; values[1] = mode_h; return mode_w != 0; }
 assert(tag == 0x30002 && count == 2 && values[0] == 9);
 pixel_queries++; values[0] = pixel_reply_id; values[1] = pixel_hz; return pixel_query_ok;
}
static void reset(unsigned mode) {
 memset(hd_regs, 0, sizeof hd_regs); memset(core_regs, 0, sizeof core_regs);
 memset(misc_regs, 0, sizeof misc_regs); memset(dma_regs, 0, sizeof dma_regs);
 memset(&hw, 0, sizeof hw); memset(&audio_hw, 0, sizeof audio_hw);
 memset(&diagnostics, 0, sizeof diagnostics); memset(&observed, 0, sizeof observed);
 attempts = window_attempts = retry_since = retry_delay = poll_since = last_link = save_since = saves = 0;
 log_us = log_ms = log_rem = 0;
 probed = 0; card_writes = pixel_queries = 0; card_busy = false; storage_ready = true; saved[0] = 0;
 audio_dev_init();
 hsm_fw_hz = measured_hz = mode_w = mode_h = 0; scenario = mode; packet_stuck = 0; write_count = 0; now = 0; pixel_hz = 148500000; pixel_reply_id = 9; pixel_query_ok = 1;
 misc_regs[0] = 0x2d6; misc_regs[1] = 0x30e0;
 hd_regs[HD_M_CTL / 4] = 1; core_regs[HC_SCHEDULER_CONTROL / 4] = 3;
 core_regs[HC_RAM_PACKET_CONFIG / 4] = 0x02;
}
static void expect(unsigned index, uint32_t address, uint32_t value) {
 assert(writes[index].address == address && writes[index].value == value);
}
int main(void) {
 const uint32_t pixels[] = {25200000, 74250000, 148500000};
 for (unsigned index = 0; index < 3; index++) {
  reset(0); pixel_hz = pixels[index];
  assert(!hdmi_audio_init() && hw.active);
  assert(audio_hw.task == hdmi_audio_task && audio_hw.tone == hdmi_audio_tone);
  expect(0, 0x20808014, 0x8207);
  expect(1, 0x2080802c, (clk.smp_n << 8) | clk.smp_m);
  expect(2, 0x20808014, 0x3028);
  expect(3, 0x2080801c, 0x20900);
  expect(4, 0x20808018, 0x08080608);
  expect(5, 0x20902094, 0x0c000003);
  expect(6, 0x20902090, 8);
  expect(7, 0x2090209c, 0x21002003);
  expect(8, 0x209020a8, 0x01001800);
  expect(9, 0x209020ac, pixels[index] / 1000);
  expect(10, 0x209020b0, pixels[index] / 1000);
  expect(11, 0x209020a0, 0x10002);
  expect(12, 0x20902490, 0x0a0184);
  expect(13, 0x20902494, 0x0170);
  for (unsigned word = 2; word < 9; word++) expect(12 + word, 0x20902490 + word * 4, 0);
  expect(21, 0x209020a0, 0x10012);
  expect(22, 0x20007200, 0x80000000);
  expect(23, 0x20007200, 6);
  expect(24, 0x20007220, 7);
  expect(25, 0x20007204, 0xc0100000);
  assert(write_count == 27 && cbs[0].dst == 0x7e808020);
  assert(cbs[0].ti == 0x00112149 && cbs[0].len == 3072);
  unsigned previous_writes = write_count;
  assert(!hdmi_audio_init() && write_count == previous_writes);
 }
 for (unsigned mode = 1; mode <= 4; mode++) {
  if (mode == 2) continue; /* position advancing without IRQ is now accepted: below */
  reset(mode); assert(hdmi_audio_init() == -1 && !hw.active);
  assert(!audio_hw.task && !audio_hw.tone && audio_hw.report == hdmi_audio_report);
  if (mode == 1) assert(strstr(why, "not draining"));
  if (mode == 3) assert(!strcmp(why, "DMA error"));
  if (mode == 4) assert(!strcmp(why, "DMA reset timed out"));
  assert(!(hd_regs[HD_MAI_CTL / 4] & 8u));
 }
 /* Scenario 2: DMA advances but no IRQ is ever seen. Accepted on the first attempt, by polling. */
 reset(2); assert(!hdmi_audio_init() && hw.active && attempts == 1 && poll_mode && !strcmp(detect, "poll"));
 assert(audio_hw.task == hdmi_audio_task && completions == 3);
 assert(strstr(diagnostics.text, "dma_detect=poll"));
 /* Scenario 5: INT bit set, no IRQ: also accepted by polling. */
 reset(5); assert(!hdmi_audio_init() && hw.active && poll_mode && completions >= 1);
 /* Healthy IRQ path reports irq. */
 reset(0); assert(!hdmi_audio_init() && !poll_mode && !strcmp(detect, "irq"));
 /* A jittery measured clock (148499000) is not a failure: N/CTS computed. */
 reset(0); pixel_hz = 148499000; assert(!hdmi_audio_init() && hw.active && clk.n == 6144 && clk.cts == 148499);
 reset(0); pixel_hz = 74250400; assert(!hdmi_audio_init() && clk.n == 6144 && clk.cts == 74250);
 reset(0); pixel_hz = 100000000; assert(hdmi_audio_init() == -1 && strstr(why, "no N/CTS"));
 reset(0); core_regs[HC_SCHEDULER_CONTROL / 4] = 1;
 assert(hdmi_audio_init() == -1 && strstr(why, "DVI mode") && !write_count);
 reset(0); pixel_hz = 0;
 assert(hdmi_audio_init() == -1 && strstr(why, "pixel clock unavailable") && !write_count);
 /* Firmware clock reads 0: measured rate, then mode table, else null. */
 reset(0); pixel_hz = 0; measured_hz = 148500000;
 assert(hdmi_audio_init() == 0 && hw.active && clk.cts);
 assert(!strcmp(observed.pixel_src, "firmware-measured") && observed.pixel_used == 148500000);
 reset(0); pixel_hz = 0; measured_hz = 3000;
 assert(hdmi_audio_init() == -1 && strstr(why, "pixel clock unavailable"));
 reset(0); pixel_hz = 0; mode_w = 1920; mode_h = 1080;
 assert(hdmi_audio_init() == 0 && hw.active && observed.pixel_used == 148500000 && !strcmp(observed.pixel_src, "mode-table"));
 reset(0); pixel_hz = 0; mode_w = 1280; mode_h = 720;
 assert(hdmi_audio_init() == 0 && observed.pixel_used == 74250000);
 reset(0); pixel_hz = 0; mode_w = 960; mode_h = 540; /* scaled framebuffer */
 assert(hdmi_audio_init() == 0 && observed.pixel_used == 148500000);
 reset(0); pixel_hz = 0; mode_w = 720; mode_h = 480;
 assert(hdmi_audio_init() == 0 && observed.pixel_used == 27000000);
 reset(0); pixel_hz = 0; mode_w = 123; mode_h = 77; /* nothing plausible */
 assert(hdmi_audio_init() == -1 && !hw.active && strstr(why, "pixel clock unavailable"));
 reset(0); pixel_hz = 0; measured_hz = 148500000; core_regs[HC_HOTPLUG / 4] = 0; /* hotplug=0 alone does not block */
 assert(hdmi_audio_init() == 0 && hw.active);
 reset(0); misc_regs[0] &= ~0x80u;
 assert(hdmi_audio_init() == -1 && strstr(why, "HSM clock stopped") && !write_count);
 for (unsigned status = 0; status <= 0x10; status += 0x10) {
  reset(0); packet_stuck = 1; core_regs[HC_RAM_PACKET_STATUS / 4] = status;
  assert(hdmi_audio_init() == -1 && strstr(why, "infoframe did not start/stop"));
  assert(!hw.active && !audio_hw.task && !(hd_regs[HD_MAI_CTL / 4] & 8u));
  assert(now <= 110000); /* 100 ms packet wait plus diagnostic timestamps */
 }
 /* Pixel-clock query initially fails, then succeeds after frames have run.
  * Do not install a task hook until success: null PCM consumption continues. */
 reset(0); pixel_hz = 0; pixel_query_ok = 0;
 assert(hdmi_audio_init() == -1 && pixel_queries == 1 && card_writes == 1);
 assert(strstr(saved, "AUDIO v2 boot=7 mode=keyboard gen=7 dropped_lines=0\n") && renames == 2);
 assert(strstr(saved, "backend=null reason=firmware pixel clock unavailable attempt=1\n"));
 assert(strstr(saved, "pixel_tag=0x00030002 clock_id=9 ok=0 reply_id=9 pixel_hz=0 mailbox=MBOX TAG\n"));
 assert(strstr(saved, "scheduler=3 hotplug=0 mode=hdmi\n"));
 assert(strstr(saved, "requested_rate=48000 sample_hz=0 channels=2 channel_status_source=kernel-iec958\n"));
 assert(strstr(saved, "channel_status_left="));
 assert(!audio_dev_open());
 uint32_t samples[48] = {0}; assert(audio_dev_write(samples, sizeof samples) == 48);
 audio_null_task(); now += 1000; audio_null_task();
 assert(audio0.ring.played == 48 && !audio_hw.task);
 pixel_hz = 148500000; pixel_query_ok = 1;
 now = retry_since + RETRY_FIRST_US - 10000; hdmi_audio_poll(); assert(pixel_queries == 1);
 now = retry_since + RETRY_FIRST_US + 100000; hdmi_audio_poll();
 assert(hw.active && pixel_queries == 2 && audio_hw.task == hdmi_audio_task);
 now += 1000000; hdmi_audio_poll();
 assert(strstr(saved, "backend=hdmi reason=ok attempt=2\n"));
 assert(strstr(saved, "pixel_hz=148500000 mailbox=ok\n"));
 assert(strstr(saved, "requested_rate=48000 sample_hz=48000"));
 /* An HPD edge causes a fresh bounded window after an exhausted boot probe. */
 reset(0); pixel_hz = 0; assert(hdmi_audio_init() == -1);
 for (unsigned i = 0; i < 100; i++) { now += 600000; hdmi_audio_poll(); }
 assert(!hw.active && pixel_queries == RETRY_WINDOW && !audio_hw.task);
 assert(strstr(saved, "retry limit reached; null drain continues"));
 assert(strstr(saved, "ok=1 reply_id=9 pixel_hz=0 mailbox=ok\n"));
 unsigned previous_queries = pixel_queries;
 now += 1000000; hdmi_audio_poll(); assert(pixel_queries == previous_queries);
 core_regs[HC_HOTPLUG / 4] = HOTPLUG_CONNECTED; now += 200000; hdmi_audio_poll();
 assert(pixel_queries == previous_queries); /* settle the new connection first */
 pixel_hz = 148500000; now += 400000; hdmi_audio_poll();
 assert(hw.active && pixel_queries == previous_queries + 1);
 /* Disconnect disables the old DMA/IRQ/hooks before any new clock query. */
 core_regs[HC_HOTPLUG / 4] = 0; now += 200000; hdmi_audio_poll();
 assert(!hw.active && !audio_hw.task && !audio_hw.tone);
 assert(!(hd_regs[HD_MAI_CTL / 4] & MAI_CTL_ENABLE));
 assert(!(core_regs[HC_RAM_PACKET_CONFIG / 4] & (1u << PKT_ID_AUDIO_INFO)));
 assert(strstr(diagnostics.text, "backend=null reason=HDMI hotplug/mode changed"));
 /* Reject a response for the wrong clock even if its rate looks usable. */
 reset(0); pixel_reply_id = 3;
 assert(hdmi_audio_init() == -1 && !hw.active && !write_count);
 assert(strstr(saved, "ok=1 reply_id=3 pixel_hz=148500000 mailbox=ok\n"));
 /* Firmware never supplies a clock, even through repeated cable edges.
  * Mailbox work is bounded per link edge (not per boot, so a TV power cycle
  * later still retries); card writes stop at their own lifetime cap. */
 reset(0); pixel_hz = 0; assert(hdmi_audio_init() == -1);
 for (unsigned i = 0; i < 200; i++) {
  if (i % 15 == 0) core_regs[HC_HOTPLUG / 4] ^= HOTPLUG_CONNECTED;
  now += 600000; hdmi_audio_poll();
 }
 assert(pixel_queries > 48 && !hw.active && !audio_hw.task);
 assert(card_writes <= AUDIO_SAVE_MAX);
 /* Busy file transfers defer discovery/writes, not the null task. */
 reset(0); pixel_hz = 0; assert(hdmi_audio_init() == -1);
 card_busy = true; now += 1000000; hdmi_audio_poll();
 assert(pixel_queries == 1 && card_writes == 1);
 card_busy = false; pixel_hz = 148500000; now += 200000; hdmi_audio_poll(); assert(hw.active);
 /* K8: no kernel SD save while the OS runs (a slow write would miss the OS heartbeat). */
 reset(0); pixel_hz = 0; os_on = true; assert(hdmi_audio_init() == -1);
 now += 3000000; hdmi_audio_poll(); now += 3000000; hdmi_audio_poll(); assert(card_writes == 0);
 os_on = false; now += 3000000; hdmi_audio_poll(); assert(card_writes == 1);
 /* CEC's EDID reads are included in the same persisted session. */
 uint8_t edid[128] = {2, 3, 8, 0x40, 0x23, 0x09, 0x07, 0x07};
 hdmi_audio_edid(1, edid); now += 1000000; hdmi_audio_poll();
 assert(strstr(saved, "edid_block=1 cta_basic_audio=1\n"));
 assert(strstr(saved, "edid_block=1 audio_data_block=090707\n"));
 /* Active audio logs two "stat" lines every 5 s and clears sticky MAI flags it saw. */
 reset(0); pixel_hz = 148500000; assert(hdmi_audio_init() == 0 && hw.active);
 for (unsigned i = 0; i < 7000; i++) {
  write_count = 0; if (i == 3) hd_regs[HD_MAI_CTL / 4] = MAI_CTL_ERRORE | MAI_CTL_EMPTY;
  else hd_regs[HD_MAI_CTL / 4] &= ~(MAI_CTL_ERRORE | MAI_CTL_DLATE | MAI_CTL_ERRORF); /* write-one-to-clear */
  completions += (i % 8 == 0); dma_regs[3] = 0xc0200000u + (i * 384u) % 12288u; hdmi_audio_poll(); }
 assert(strstr(diagnostics.text, " stat rate dt_ms=") && strstr(diagnostics.text, " stat flags looks="));
 assert(strstr(diagnostics.text, "underflow=1 "));
 puts("hdmi startup ok");
 return 0;
}
