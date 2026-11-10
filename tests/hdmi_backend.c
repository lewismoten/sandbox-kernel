/* SPDX-License-Identifier: MIT; regression witnesses adapted from run586.
 * Exact production fill/task/builder bodies over explicit DMA register states. */
#include <assert.h>
#include <string.h>
#include "audio_null.h"
#include "hdmi_audio.h"
#include "mixer.h"
#include "iec958.h"
#define PERIPH 0x20000000u
#define SEG_WORDS (HDMI_DMA_SEG_FRAMES * 2u)
#define DMA_CONBLK 4u
#define DMA_CS 0u
static struct { int active, ch; } hw;
static struct dma_cb cbs[HDMI_DMA_SEGS];
static uint32_t buf[HDMI_DMA_SEGS * SEG_WORDS], pcm[HDMI_DMA_SEG_FRAMES];
static uint8_t stL[24], stR[24];
static struct mixer mx;
static volatile uint32_t completions;
static uint32_t last_completed, segs_played, starved, errors, fake_cs, fake_us;
static uint32_t last_progress_us;
static int stalled;
static unsigned dma_writes;
static uint32_t micros(void) { return fake_us; }
uint64_t clock_us(void) { return 0; }
static uint32_t bus(const void *p) { return p == cbs ? 0x100000u : 0x200000u; }
static uint32_t dma(uint32_t o) { assert(o == DMA_CS); return fake_cs; }
static void dmaw(uint32_t o, uint32_t v) {
 assert(o == DMA_CS); /* task/IRQ must never rewrite CONBLK */
 dma_writes++;
 /* Model CS control bits plus write-one-to-clear END/INT/ERROR. */
 fake_cs = (fake_cs & (0x106u & ~v)) | (v & ~0x106u);
}
void hdmi_audio_irq(void);
static void complete(unsigned n) {
 for (unsigned i = 0; i < n; i++) {
  fake_cs |= 6u;
  hdmi_audio_irq();
  assert((fake_cs & 1u) && !(fake_cs & 4u));
 }
}
static void drain(void) {}
#include "hdmi-functions.h"

int main(void) {
 audio_dev_init(); assert(audio_dev_open() == 0); mixer_init(&mx);
 assert(!iec958_status(48000, 0, stL)); assert(!iec958_status(48000, 1, stR));
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

 build_ring();
 for (unsigned i = 0; i < HDMI_DMA_SEGS; i++) {
  assert(cbs[i].next == bus(cbs) + ((i + 1) % HDMI_DMA_SEGS) * sizeof(*cbs));
  assert(cbs[i].ti & DMA_TI_INTEN);
 }
 hw.active = 1; hw.ch = 1; fake_cs = 1u | (8u << 16) | (15u << 20);
 hdmi_audio_irq(); assert(!completions && !dma_writes); /* no pending IRQ */
 uint32_t samples[HDMI_DMA_SEG_FRAMES];
 for (unsigned i = 0; i < HDMI_DMA_SEG_FRAMES; i++) samples[i] = sample;
 assert(audio_dev_write(samples, sizeof samples) == HDMI_DMA_SEG_FRAMES);
 complete(1); hdmi_audio_task();
 assert(completions == 1 && segs_played == 1 && !starved);
 assert(audio0.ring.played == HDMI_DMA_SEG_FRAMES && !audio0.ring.underruns);
 unsigned writes = dma_writes;
 hdmi_audio_task(); assert(segs_played == 1 && !starved && dma_writes == writes);
 /* Full lap: same CB index, eight genuine completions and eight drains. */
 complete(8); hdmi_audio_task();
 assert(segs_played == 9 && starved == 1 && audio0.ring.underruns == 8);
 /* Multiple full laps must not collapse into no progress or just one lap. */
 complete(24); hdmi_audio_task();
 assert(segs_played == 33 && starved == 2 && audio0.ring.underruns == 32);
 assert(dma_writes == 33); /* IRQ acknowledges only; task never restarts DMA */
 /* A real stall counts once after 64 ms, without inventing completions. */
 fake_us = 63999; hdmi_audio_task(); assert(!stalled && starved == 2);
 fake_us = 64000; hdmi_audio_task(); assert(stalled && starved == 3);
 fake_us = 128000; hdmi_audio_task();
 assert(segs_played == 33 && starved == 3 && audio0.ring.underruns == 32);
 complete(1); hdmi_audio_task(); assert(!stalled && segs_played == 34);
 fake_us += 64000; hdmi_audio_task(); assert(stalled && starved == 4);
 /* Counter wrap also preserves an eight-segment lap. */
 completions = last_completed = UINT32_MAX - 3u;
 complete(8); hdmi_audio_task();
 assert(completions == 4 && segs_played == 42 && !stalled && starved == 5);
 assert(audio0.ring.underruns == 41);
 assert((fake_cs & 1u) && ((fake_cs >> 16) & 15u) == 8u);
 return 0;
}
