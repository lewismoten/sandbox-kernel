/* SPDX-License-Identifier: MIT; regression witnesses adapted from run586.
 * Exact production fill/task/builder bodies over explicit DMA register states. */
#include <assert.h>
#include <string.h>
#include "audio_null.h"
#include "clock.h"
#include "hdmi_audio.h"
#include "mixer.h"
#include "iec958.h"
#include "audio_stats.h"
#define PERIPH 0x20000000u
#define SEG_WORDS (HDMI_DMA_SEG_FRAMES * 2u)
#define DMA_CONBLK 4u
#define DMA_CS 0u
static struct { int active, ch; } hw;
static struct dma_cb cbs[HDMI_DMA_SEGS];
static uint32_t silence[SEG_WORDS], packed[SEG_WORDS];
static volatile uint8_t ready[HDMI_DMA_SEGS];
static volatile uint32_t irq_starved;
static uint32_t current = 6;
static int irq_masked, interrupt_before_publish;
static uint32_t buf[HDMI_DMA_SEGS * SEG_WORDS], pcm[HDMI_DMA_SEG_FRAMES];
static uint8_t stL[24], stR[24];
static struct mixer mx;
static volatile uint32_t completions;
static uint32_t last_completed, segs_played, starved, errors, fake_cs, fake_us;
static uint32_t last_progress_us;
static int stalled;
static unsigned dma_writes;
static uint32_t pos_idx;
static int poll_mode;
static uint32_t micros(void) { return fake_us; }
uint64_t clock_us(void) { return 0; }
static uint32_t bus(const void *p) { return p == cbs ? 0x100000u : 0x200000u; }
static uint32_t dma(uint32_t o) { if (o == DMA_CONBLK) return bus(cbs) + current * sizeof(*cbs); assert(o == DMA_CS); return fake_cs; }
static void dmaw(uint32_t o, uint32_t v) {
 assert(o == DMA_CS); /* task/IRQ must never rewrite CONBLK */
 dma_writes++;
 /* Model CS control bits plus write-one-to-clear END/INT/ERROR. */
 fake_cs = (fake_cs & (0x106u & ~v)) | (v & ~0x106u);
}
void hdmi_audio_irq(void);
static void complete(unsigned n) {
 for (unsigned i = 0; i < n; i++) {
  assert(!irq_masked);
  current = (current + 1u) % HDMI_DMA_SEGS;
  fake_us += 8000;
  fake_cs |= 6u;
  hdmi_audio_irq();
  assert((fake_cs & 1u) && !(fake_cs & 4u));
 }
}
static uint32_t irq_save(void) {
 assert(!irq_masked);
 if (interrupt_before_publish) { interrupt_before_publish = 0; complete(1); }
 irq_masked = 1; return 0;
}
static void irq_restore(uint32_t flags) { assert(!flags && irq_masked); irq_masked = 0; }
static void drain(void) {}
static void reset_dma(void) {
 completions = last_completed = segs_played = starved = irq_starved = 0;
 fake_us = last_progress_us = 0; stalled = 0; current = 0; dma_writes = 0;
 for (unsigned i = 0; i < HDMI_DMA_SEGS; i++) {
  memcpy(buf + i * SEG_WORDS, silence, sizeof silence); ready[i] = 1;
 }
}

#include "hdmi-functions.h"

int main(void) {
 audio_dev_init(); assert(audio_dev_open() == 0); mixer_init(&mx);
 assert(!iec958_status(48000, 0, stL)); assert(!iec958_status(48000, 1, stR));
 iec958_pack(pcm, HDMI_DMA_SEG_FRAMES, 0, stL, stR, silence);
 memset(pcm, 0xff, sizeof pcm);
 for (unsigned i = 0; i < 16; i++) fill(i % HDMI_DMA_SEGS);
 assert(!audio0.wrote && !audio0.ring.underruns && !audio0.ring.played);
 for (unsigned i = 0; i < HDMI_DMA_SEG_FRAMES; i++) assert(pcm[i] == 0);
 uint32_t sample = 0x12345678u;
 assert(audio_dev_write(&sample, sizeof sample) == 1);
 fill(0);
 assert(pcm[0] == sample && pcm[1] == 0);
 assert(audio0.ring.played == 1 && audio0.ring.underruns == 1);
 fill(1); assert(audio0.ring.underruns == 2);
 audio_dev_close(); fill(2); assert(audio0.ring.underruns == 2);
 assert(audio_dev_open() == 0); fill(3); assert(!audio0.ring.underruns);
 /* Asleep with an OS that writes nothing: DMA segments are silence, not a glitch storm. */
 assert(audio_dev_write(&sample, sizeof sample) == 1); audio0.asleep = 1;
 for (unsigned i = 0; i < 8; i++) fill(i % HDMI_DMA_SEGS);
 assert(audio0.ring.underruns == 0 && audio0.ring.played == 1);
 audio0.asleep = 0; audio_dev_close(); assert(audio_dev_open() == 0);

 build_ring();
 for (unsigned i = 0; i < HDMI_DMA_SEGS; i++) {
  assert(cbs[i].next == bus(cbs) + ((i + 1) % HDMI_DMA_SEGS) * sizeof(*cbs));
  assert(cbs[i].ti & DMA_TI_INTEN);
 }
 hw.active = 1; hw.ch = 1; fake_cs = 1u | (8u << 16) | (15u << 20);
 reset_dma();
 hdmi_audio_irq(); assert(!completions && !dma_writes);
 uint32_t samples[HDMI_DMA_SEG_FRAMES], expected[SEG_WORDS];
 for (unsigned i = 0; i < HDMI_DMA_SEG_FRAMES; i++) samples[i] = sample;
 iec958_pack(samples, HDMI_DMA_SEG_FRAMES, 0, stL, stR, expected);
 /* Normal service: refill each completed buffer, then hear it exactly once
  * on its next lap. IRQ retirement must preserve every other DMA buffer. */
 for (unsigned tick = 0; tick < 32; tick++) {
  assert(!memcmp(buf + current * SEG_WORDS, tick < 8 ? silence : expected, sizeof expected));
  uint32_t before[HDMI_DMA_SEGS * SEG_WORDS]; memcpy(before, buf, sizeof buf);
  unsigned done = current;
  complete(1);
  assert(!memcmp(buf + done * SEG_WORDS, silence, sizeof silence));
  for (unsigned j = 0; j < HDMI_DMA_SEGS; j++) if (j != done)
   assert(!memcmp(buf + j * SEG_WORDS, before + j * SEG_WORDS, sizeof silence));
  assert(audio_dev_write(samples, sizeof samples) == HDMI_DMA_SEG_FRAMES);
  service();
  assert(!memcmp(buf + done * SEG_WORDS, expected, sizeof expected));
 }
 assert(segs_played == 32 && !starved && !irq_starved && !audio0.ring.underruns);
 /* Seed all eight segments with sound; 192 ms without any task service
  * plays that sound once, followed by 128 ms of properly packed silence. */
 reset_dma();
 for (unsigned i = 0; i < HDMI_DMA_SEGS; i++) memcpy(buf + i * SEG_WORDS, expected, sizeof expected);
 for (unsigned tick = 0; tick < 24; tick++) {
  assert(!memcmp(buf + current * SEG_WORDS, tick < 8 ? expected : silence, sizeof expected));
  complete(1);
  assert(irq_starved == (tick < 8 ? 0 : tick - 7));
 }
 assert(!segs_played && irq_starved == 16); /* counted before foreground resumes */
 service();
 assert(segs_played == 24 && !starved && audio0.ring.underruns == 24);
 for (unsigned i = 0; i < HDMI_DMA_SEGS; i++) assert(!memcmp(buf + i * SEG_WORDS, silence, sizeof silence));
 /* DMA with no progress counts once per stall; does not invent drains. */
 fake_us += 63999; service(); assert(!stalled && !starved);
 fake_us++; service(); assert(stalled && starved == 1);
 fake_us += 64000; service(); assert(starved == 1 && segs_played == 24);
 /* Counter wrap retains full laps. */
 completions = last_completed = UINT32_MAX - 3u;
 complete(8); service();
 assert(completions == 4 && segs_played == 32 && !stalled);
 assert(audio0.ring.underruns == 32);
 assert((fake_cs & 1u) && ((fake_cs >> 16) & 15u) == 8u);
 /* A task that packed too late must leave current and next untouched. */
 for (unsigned offset = 0; offset < 2; offset++) {
  unsigned seg = (current + offset) % HDMI_DMA_SEGS;
  uint32_t before[SEG_WORDS]; memcpy(before, buf + seg * SEG_WORDS, sizeof before);
  assert(audio_dev_write(samples, sizeof samples) == HDMI_DMA_SEG_FRAMES);
  fill(seg);
  assert(!memcmp(before, buf + seg * SEG_WORDS, sizeof before));
 }
 /* An IRQ between packing and publication must not corrupt staged PCM,
  * and ownership must be checked against the advanced DMA position. */
 unsigned target = (current + 2u) % HDMI_DMA_SEGS;
 uint32_t before[SEG_WORDS]; memcpy(before, buf + target * SEG_WORDS, sizeof before);
 assert(audio_dev_write(samples, sizeof samples) == HDMI_DMA_SEG_FRAMES);
 interrupt_before_publish = 1; fill(target);
 assert(target == (current + 1u) % HDMI_DMA_SEGS);
 assert(!memcmp(before, buf + target * SEG_WORDS, sizeof before));
 target = (current + HDMI_DMA_SEGS - 1u) % HDMI_DMA_SEGS;
 assert(audio_dev_write(samples, sizeof samples) == HDMI_DMA_SEG_FRAMES);
 interrupt_before_publish = 1; fill(target);
 assert(!memcmp(buf + target * SEG_WORDS, expected, sizeof expected));
 assert(ready[target]);
 /* IRQ never delivered: polling counts the DMA position advancing; a frozen DMA counts nothing. */
 reset_dma(); poll_mode = 1; pos_idx = 0; fake_cs = 1u | (8u << 16) | (15u << 20);
 service(); assert(!completions);
 current = 3; service(); assert(completions == 3 && pos_idx == 3);
 service(); assert(completions == 3);
 current = 1; fake_cs |= 6u; service(); assert(completions == 9 && !(fake_cs & 4u) && pos_idx == 1);
 poll_mode = 0;
 /* The foreground stalls 400 ms (USB host work, a GMOS frame): the 10 ms timer tick alone refills,
  * so the DMA keeps consuming 48 kHz stereo (384000 bytes/s) with no starvation. */
 reset_dma(); poll_mode = 1; pos_idx = 0; fake_cs = 1u | (8u << 16) | (15u << 20);
 {
  static uint32_t loud[AUDIO_RING_FRAMES]; for (unsigned i = 0; i < AUDIO_RING_FRAMES; i++) loud[i] = sample;
  audio_dev_close(); assert(audio_dev_open() == 0);
  assert(audio_dev_write(loud, 4u * HDMI_DMA_SEG_FRAMES * 40u) == HDMI_DMA_SEG_FRAMES * 40u);
  struct audio_stats st; audio_stats_begin(&st, 0, 0);
  uint32_t ring_us = 0, done_us = 0; unsigned segs_before = segs_played;
  for (uint32_t t = 10000; t <= 5000000u; t += 10000) { /* one timer tick every 10 ms */
   while (done_us + 8000 <= t) { done_us += 8000; current = (current + 1u) % HDMI_DMA_SEGS; fake_cs |= 6u; }
   fake_us = t; ring_us = t; service();
   audio_stats_sample(&st, t, current * SEG_WORDS * 4u, sizeof buf, 0, 0, 0);
   if (t == 100000) { /* the foreground then stalls: nothing refills the ring except the tick */ }
  }
  (void)ring_us;
  assert(segs_played - segs_before >= 620 && segs_played - segs_before <= 626); /* 5 s / 8 ms = 625 */
  assert(!irq_starved && !starved && audio0.ring.underruns <= 625u - 40u + 1u);
  assert(st.max_gap_us == 10000u && !st.big_gaps);
  uint32_t bps = st.bytes / 5000u * 1000u;
  assert(bps >= 380000u && bps <= 388000u);
 }
 return 0;
}
