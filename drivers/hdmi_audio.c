/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). HDMI audio backend: the MAI FIFO fed by a
 * looping DMA control-block ring that this file refills from /dev/audio's ring.
 * Replaces the null backend when hdmi_audio_init succeeds; otherwise the null
 * backend keeps running and the reason is logged. Sources and the VERIFY
 * convention are in include/hdmi_audio.h. */
#include "kernel.h"
#include "audio_null.h"
#include "hdmi_audio.h"
#include "iec958.h"
#include "mixer.h"

#define DMA_BASE (PERIPH + 0x7000u)
#define DMA_ENABLE (DMA_BASE + 0xff0u)
#define CM_HSMCTL (PERIPH + 0x101000u + 0x088u) /* Clock Manager HSM clock; offsets from the Linux clock driver. VERIFY */
#define CM_HSMDIV (PERIPH + 0x101000u + 0x08cu)
#define DMA_CS 0x00u
#define DMA_CONBLK 0x04u
#define DMA_DEBUG 0x20u
#define TONE_VOL 26u /* Q8: about 10% of full scale */
#define PIXEL_HZ 148500000u /* CEA mode 16: 1080p60, set by config.txt */
#define RATE 48000u
#define SEG_WORDS (HDMI_DMA_SEG_FRAMES * 2u)

static struct { int active, ch; uint32_t cs; } hw;
static struct dma_cb cbs[HDMI_DMA_SEGS] __attribute__((aligned(32)));
static uint32_t buf[HDMI_DMA_SEGS * SEG_WORDS] __attribute__((aligned(32)));
static uint32_t pcm[HDMI_DMA_SEG_FRAMES];
static uint8_t stL[24], stR[24];
static struct mixer mx;
static uint32_t last_cur, ahead, segs_played, starved, started_us, errors, tones;
static struct hdmi_audio_clocks clk;
static const char *why = "not tried";

static uint32_t bus(const void *p) { return DMA_BUS_RAM | (uint32_t)(uintptr_t)p; }
static void drain(void) { uint32_t z = 0; __asm__ volatile("mcr p15,0,%0,c7,c10,4" :: "r"(z) : "memory"); } /* write buffer: the DMA reads RAM (write-through) */
static void hd(uint32_t o, uint32_t v) { REG(HDMI_HD_BASE + o) = v; barrier(); }
static void hc(uint32_t o, uint32_t v) { REG(HDMI_CORE_BASE + o) = v; barrier(); }
static uint32_t dma(uint32_t o) { return REG(DMA_BASE + (uint32_t)hw.ch * 0x100u + o); }
static void dmaw(uint32_t o, uint32_t v) { REG(DMA_BASE + (uint32_t)hw.ch * 0x100u + o) = v; barrier(); }

static void say(const char *a, uint32_t n, const char *b) {
 char l[96], t[12], *p = l; const char *s;
 for (s = "AUDIO hdmi: "; *s;) *p++ = *s++;
 for (s = a; *s;) *p++ = *s++;
 number(t, n); for (char *q = t; *q;) *p++ = *q++;
 for (s = b; *s;) *p++ = *s++;
 *p = 0; log_line(l);
}
static void hex(char *o, uint32_t v) { for (int i = 7; i >= 0; i--) *o++ = "0123456789abcdef"[(v >> (4 * i)) & 15]; *o = 0; }
static void sayhex(const char *a, uint32_t v) { char l[80], h[9], *p = l; const char *s; hex(h, v); for (s = "AUDIO hdmi: "; *s;) *p++ = *s++; for (s = a; *s;) *p++ = *s++; for (char *q = h; *q;) *p++ = *q++; *p = 0; log_line(l); }
static int fail(const char *w) { why = w; log_line("AUDIO hdmi: unavailable, null backend stays:"); log_line(w); return -1; }

static void fill(uint32_t seg) {
 uint32_t q = audio_ring_queued(&audio0.ring), n = audio0.open ? HDMI_DMA_SEG_FRAMES : (q < HDMI_DMA_SEG_FRAMES ? q : HDMI_DMA_SEG_FRAMES);
 if (n) audio_ring_read(&audio0.ring, pcm, n); /* open: a short queue counts one underrun and pads with silence */
 for (uint32_t i = n; i < HDMI_DMA_SEG_FRAMES; i++) pcm[i] = 0;
 iec958_pack(pcm, HDMI_DMA_SEG_FRAMES, 0, stL, stR, buf + seg * SEG_WORDS);
 /* The segment always starts a block: 384 = 2 * 192, so the frame counter restarts at 0. */
}

int hdmi_audio_init(void) {
 if (hw.active) return 0;
 uint32_t m[1] = {0};
 if (!property(0x60001, m, 1)) return fail("DMA channel mask unavailable");
 hw.ch = -1; for (int c = 1; c <= 6; c++) if (m[0] & (1u << c)) { hw.ch = c; break; }
 if (hw.ch < 0) return fail("no free DMA channel 1-6");
 uint32_t hsm = hdmi_hsm_hz(REG(CM_HSMCTL), REG(CM_HSMDIV)), assumed = 0;
 sayhex("CM_HSMCTL ", REG(CM_HSMCTL)); sayhex("CM_HSMDIV ", REG(CM_HSMDIV));
 if (!hsm) { hsm = 163682000u; assumed = 1; } /* unknown source: the rate Linux comments name for this clock; VERIFY */
 if (hdmi_audio_clocks(RATE, PIXEL_HZ, hsm, &clk)) return fail("no N/CTS for this mode");
 if (iec958_status(RATE, 0, stL) || iec958_status(RATE, 1, stR)) return fail("channel status");
 if (!(REG(HDMI_HD_BASE + HD_M_CTL) & 1u)) return fail("HD block not enabled by firmware (is HDMI on?)");
 say("HSM ", hsm, assumed ? " Hz (assumed)" : " Hz (from CM)");
 say("N ", clk.n, ""); say("CTS ", clk.cts, ""); say("SMP N ", clk.smp_n, ""); say("SMP M ", clk.smp_m, ""); say("sample clock ", clk.mai_hz, " Hz");
 /* MAI: reset and flush, then the 2-channel PCM setup. */
 hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);
 hd(HD_MAI_CTL, MAI_CTL_ERRORF); hd(HD_MAI_CTL, MAI_CTL_FLUSH);
 hc(HC_MAI_CHANNEL_MAP, hdmi_mai_channel_map());
 hc(HC_MAI_CONFIG, MAI_CONFIG_BIT_REVERSE | MAI_CONFIG_FORMAT_REVERSE | 0x3u);
 hd(HD_MAI_FMT, hdmi_mai_fmt(RATE));
 hd(HD_MAI_THR, hdmi_mai_thr());
 hd(HD_MAI_SMP, (clk.smp_n << 8) | clk.smp_m);
 hc(HC_AUDIO_PACKET_CONFIG, hdmi_audio_packet_config());
 hc(HC_CRP_CFG, hdmi_crp_cfg(clk.n)); hc(HC_CTS_0, clk.cts); hc(HC_CTS_1, clk.cts);
 /* Audio InfoFrame into packet RAM slot 4, then enable it (read-modify-write: the firmware's other packets stay). */
 {
  uint8_t f[AUDIO_INFOFRAME_BYTES]; uint32_t w[9], cfg = REG(HDMI_CORE_BASE + HC_RAM_PACKET_CONFIG), slot = HC_RAM_PACKET_START + HC_PACKET_STRIDE * PKT_ID_AUDIO_INFO;
  hdmi_audio_infoframe(f); hdmi_infoframe_words(f, sizeof f, w);
  hc(HC_RAM_PACKET_CONFIG, cfg & ~(1u << PKT_ID_AUDIO_INFO));
  for (uint32_t s = micros(); (REG(HDMI_CORE_BASE + HC_RAM_PACKET_STATUS) >> PKT_ID_AUDIO_INFO) & 1u; ) if (expired(s, 100000)) break;
  for (unsigned i = 0; i < 9; i++) hc(slot + 4 * i, w[i]);
  hc(HC_RAM_PACKET_CONFIG, cfg | (1u << PKT_ID_AUDIO_INFO));
  sayhex("RAM_PACKET_CONFIG ", REG(HDMI_CORE_BASE + HC_RAM_PACKET_CONFIG));
 }
 /* Silence in every segment, then the DMA ring. */
 mixer_init(&mx);
 for (uint32_t i = 0; i < HDMI_DMA_SEGS; i++) { for (uint32_t k = 0; k < HDMI_DMA_SEG_FRAMES; k++) pcm[k] = 0; iec958_pack(pcm, HDMI_DMA_SEG_FRAMES, 0, stL, stR, buf + i * SEG_WORDS); }
 dma_build_ring(cbs, HDMI_DMA_SEGS, bus(cbs), bus(buf), SEG_WORDS * 4u, 0x7e000000u | (HDMI_HD_BASE - PERIPH + HD_MAI_DATA));
 drain();
 hd(HD_MAI_CTL, hdmi_mai_ctl_run());
 REG(DMA_ENABLE) |= 1u << hw.ch; barrier();
 dmaw(DMA_CS, 0x80000000u); for (uint32_t s = micros(); dma(DMA_CS) & 0x80000000u; ) if (expired(s, 10000)) break;
 dmaw(DMA_CS, 0x6u); /* clear END and INT */
 dmaw(DMA_CONBLK, bus(cbs));
 started_us = micros(); last_cur = 0; ahead = HDMI_DMA_SEGS - 1; segs_played = 0; starved = 0; errors = 0;
 dmaw(DMA_CS, 1u | (1u << 28) | (8u << 16) | (15u << 20)); /* ACTIVE, wait for outstanding writes, priority 8, panic 15 */
 delay_us(30000);
 uint32_t c1 = dma(DMA_CONBLK), mc = REG(HDMI_HD_BASE + HD_MAI_CTL);
 sayhex("DMA CS ", dma(DMA_CS)); sayhex("DMA DEBUG ", dma(DMA_DEBUG)); sayhex("MAI_CTL ", mc);
 if ((dma(DMA_CS) & 0x100u) || c1 == bus(cbs)) {
  dmaw(DMA_CS, 0x80000000u); hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH);
  return fail((dma(DMA_CS) & 0x100u) ? "DMA error" : "DMA is not draining (DREQ or MAI not running)");
 }
 hw.active = 1; why = "ok";
 audio_hw.task = hdmi_audio_task; audio_hw.tone = hdmi_audio_tone; audio_hw.report = hdmi_audio_report;
 log_line("AUDIO hdmi: DMA draining; HDMI replaces the null backend (silence until /dev/audio or TONE)");
 return 0;
}
int hdmi_audio_active(void) { return hw.active; }

void hdmi_audio_task(void) {
 if (!hw.active) return;
 if (mixer_active(&mx)) mixer_pump_dev(&mx, HDMI_DMA_SEG_FRAMES);
 uint32_t cur = dma_cb_index(dma(DMA_CONBLK), bus(cbs), HDMI_DMA_SEGS);
 uint32_t adv = (cur - last_cur) & (HDMI_DMA_SEGS - 1);
 last_cur = cur; segs_played += adv;
 if (adv > ahead) { starved++; ahead = 0; } else ahead -= adv; /* a late refill replayed old data once */
 if (dma(DMA_CS) & 0x100u) { errors++; dmaw(DMA_CS, 0x100u); }
 for (; ahead < HDMI_DMA_SEGS - 1; ahead++) fill((cur + ahead + 1) & (HDMI_DMA_SEGS - 1));
 drain();
}
int hdmi_audio_tone(uint32_t hz, uint32_t ms) {
 if (!hw.active) return -1;
 if (mixer_tone(&mx, hz, ms, (uint16_t)TONE_VOL) < 0) return -1;
 tones++; return 0;
}
void hdmi_audio_report(char *out, size_t cap) {
 char t[12]; size_t n = 0; while (out[n]) n++;
 uint32_t el = (micros() - started_us) / 1000u; /* ms */
 uint32_t frames = segs_played * HDMI_DMA_SEG_FRAMES;
 uint32_t fps = el ? (frames / el) * 1000u + (frames % el) * 1000u / el : 0;
 const struct { const char *k; uint32_t v; } f[] = {
  {"backend=hdmi segs=", segs_played}, {" drain_fps=", fps}, {" starved=", starved}, {" dma_err=", errors},
  {" ring_underruns=", (uint32_t)audio0.ring.underruns}, {" ring_queued=", audio_ring_queued(&audio0.ring)}, {" tones=", tones}, {" mixer_active=", mixer_active(&mx)}};
 for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
  for (const char *s = f[i].k; *s && n + 1 < cap;) out[n++] = *s++;
  number(t, f[i].v); for (char *q = t; *q && n + 1 < cap;) out[n++] = *q++;
 }
 char h[9]; hex(h, REG(HDMI_HD_BASE + HD_MAI_CTL)); const char *l = " mai_ctl=0x"; for (; *l && n + 1 < cap;) out[n++] = *l++;
 for (char *q = h; *q && n + 1 < cap;) out[n++] = *q++;
 out[n] = 0;
}
