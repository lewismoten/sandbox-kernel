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
#include "audio_diag.h"
#include "audio_rot.h"
#include "audio_stall.h"
#include "bootinfo.h"
#include "fatput.h"
#include "os.h"

#define DMA_BASE (PERIPH + 0x7000u)
#define DMA_ENABLE (DMA_BASE + 0xff0u)
#define CM_HSMCTL (PERIPH + 0x101000u + 0x088u) /* Linux rpi-6.6.y clk-bcm2835.c: HSM */
#define CM_HSMDIV (PERIPH + 0x101000u + 0x08cu)
#define DMA_CS 0x00u
#define DMA_CONBLK 0x04u
#define DMA_DEBUG 0x20u
#define TONE_VOL 26u /* Q8: about 10% of full scale */
#define RATE 48000u
#define SEG_WORDS (HDMI_DMA_SEG_FRAMES * 2u)

static struct { int active, ch; uint32_t cs; } hw;
static struct dma_cb cbs[HDMI_DMA_SEGS] __attribute__((aligned(32)));
static uint32_t buf[HDMI_DMA_SEGS * SEG_WORDS] __attribute__((aligned(32)));
static uint32_t pcm[HDMI_DMA_SEG_FRAMES];
static uint32_t silence[SEG_WORDS], packed[SEG_WORDS];
static volatile uint8_t ready[HDMI_DMA_SEGS];
static volatile uint32_t irq_starved; /* completed segments played without a task refill */
static uint8_t stL[24], stR[24];
static struct mixer mx;
static volatile uint32_t completions; /* segment-complete IRQ, or poll_completions() when the IRQ never arrives */
static uint32_t pos_idx; /* ring segment DMA was reading at the last completion */
static int poll_mode; /* 1: completions come from polling CS/CONBLK, not the IRQ */
static const char *detect = "none";
static uint32_t last_completed, segs_played, starved, started_us, errors, tones;
static uint32_t last_progress_us;
static int stalled;
static struct audio_stall stall_watch;
static struct hdmi_audio_clocks clk;
static const char *why = "not tried";
/* Never busy-wait for a TV to wake. Initial retry is after ~15 60-Hz frames,
 * then every 500 ms. Each connector/mode edge starts a new bounded window
 * (never a per-boot cap: a TV power cycle later must still retry). */
#define RETRY_FIRST_US 250000u
#define RETRY_EVERY_US 500000u
#define RETRY_WINDOW 12u
#define AUDIO_SAVE_MAX 20u
static struct audio_diag diagnostics;
static struct audio_observation observed;
static uint32_t attempts, window_attempts, retry_since, retry_delay;
static uint32_t poll_since, last_link, save_since, saves;
static int probed, display_seen, rotated;
static uint32_t log_us, log_ms, log_rem;
/* Accumulate across the 32-bit hardware timer's 71-minute wrap. Poll calls
 * this every foreground pass, even when discovery and logging are idle. */
static uint32_t timestamp(void) {
 uint32_t now = micros(), delta = now - log_us; log_us = now;
 log_ms += delta / 1000u; log_rem += delta % 1000u;
 log_ms += log_rem / 1000u; log_rem %= 1000u;
 return log_ms;
}
extern bool storage_ready;

static void record_decision(void) {
 observed.attempt = attempts;
 observed.sample_hz = clk.mai_hz;
 audio_diag_decision(&diagnostics, timestamp(), &observed, hw.active, why);
}
extern uint32_t boot_generation;
static uint32_t link_state(void);
static int rot_rename(const char *from, const char *to, void *ctx) {
 return fatsave_rename("KERNEL", from, to, (uint8_t *)ctx, AUDIO_LOG_BYTES + 80);
}
static void save_diagnostics(void) {
 if (!diagnostics.dirty || !storage_ready || protocol_busy() || os_running() || saves >= AUDIO_SAVE_MAX) return;
 if (link_state() & 1u) display_seen = 1;
 /* A link boot (Mac attached) with no display must not replace the display boot's record. */
 if (!audio_rot_should_write(bootinfo_this(), display_seen)) return;
 if (saves && !expired(save_since, 1000000u)) return;
 save_since = micros(); saves++;
 static uint8_t out[AUDIO_LOG_BYTES + 80];
 if (!rotated) { /* first write of this boot: keep the last three boots (AUDIO.TXT, .1, .2) */
  rotated = 1; struct audio_rot_io io = {rot_rename, out}; audio_rot_rotate(&io);
 }
 /* The first line names the boot number, mode and kernel generation. */
 uint32_t n = audio_rot_header((char *)out, 80, bootinfo_this(), boot_generation, diagnostics.dropped);
 out[n++] = '\n';
 memcpy(out + n, diagnostics.text, diagnostics.len); n += diagnostics.len;
 if (fatsave_put("KERNEL", "AUDIO.TXT", out, n)) log_line("AUDIO.TXT write failed");
 else diagnostics.dirty = 0;
}
void hdmi_audio_edid(unsigned block, const uint8_t *data) {
 display_seen = 1; /* a display answered */
 audio_diag_edid(&diagnostics, timestamp(), block, data);
}
static uint32_t link_state(void) {
 return (REG(HDMI_CORE_BASE + HC_HOTPLUG) & HOTPLUG_CONNECTED) |
        ((REG(HDMI_CORE_BASE + HC_SCHEDULER_CONTROL) & SCHEDULER_HDMI_ACTIVE) << 1) |
        ((REG(HDMI_HD_BASE + HD_M_CTL) & 1u) << 3);
}

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
 *p = 0; boot_note(l);
}
static void hex(char *o, uint32_t v) { for (int i = 7; i >= 0; i--) *o++ = "0123456789abcdef"[(v >> (4 * i)) & 15]; *o = 0; }
static void sayhex(const char *a, uint32_t v) { char l[80], h[9], *p = l; const char *s; hex(h, v); for (s = "AUDIO hdmi: "; *s;) *p++ = *s++; for (s = a; *s;) *p++ = *s++; for (char *q = h; *q;) *p++ = *q++; *p = 0; boot_note(l); }
static int fail(const char *w) {
 why = w; boot_note("AUDIO hdmi: unavailable, null backend stays:"); boot_note(w);
 record_decision(); save_diagnostics(); return -1;
}

static void read_pcm(void) {
 uint32_t q = audio_ring_queued(&audio0.ring), n = (audio0.open && audio0.wrote) ? HDMI_DMA_SEG_FRAMES : (q < HDMI_DMA_SEG_FRAMES ? q : HDMI_DMA_SEG_FRAMES);
 if (n) audio_ring_read(&audio0.ring, pcm, n); /* after first write: short queues count an underrun and pad with silence */
 for (uint32_t i = n; i < HDMI_DMA_SEG_FRAMES; i++) pcm[i] = 0;
}

/* Only the bounded publication copy excludes IRQs. Packing/ring consumption
 * stays in the foreground. Preserve the caller's IRQ state. */
static uint32_t irq_save(void) {
 uint32_t flags, masked;
 __asm__ volatile("mrs %0,cpsr\n\torr %1,%0,#0x80\n\tmsr cpsr_c,%1"
                  : "=r"(flags), "=r"(masked) :: "memory", "cc");
 return flags;
}
static void irq_restore(uint32_t flags) {
 __asm__ volatile("msr cpsr_c,%0" :: "r"(flags) : "memory", "cc");
}
static void fill(uint32_t seg) {
 read_pcm();
 iec958_pack(pcm, HDMI_DMA_SEG_FRAMES, 0, stL, stR, packed);
 /* Every segment starts a block (384 = 2 * 192). DMA keeps moving while
  * IRQs are masked: exclude both its current and next segment, leaving at
  * least 8 ms for this fixed 3 KiB copy and write-buffer drain. */
 uint32_t flags = irq_save();
 uint32_t current = dma_cb_index(dma(DMA_CONBLK), bus(cbs), HDMI_DMA_SEGS);
 if (seg != current && seg != ((current + 1u) & (HDMI_DMA_SEGS - 1u))) {
  for (uint32_t i = 0; i < SEG_WORDS; i++) buf[seg * SEG_WORDS + i] = packed[i];
  drain();
  ready[seg] = 1;
 }
 irq_restore(flags);
}

/* Keep the chain circular; INTEN reports each completed segment without
 * stopping DMA. Service the IRQ within one segment (8 ms): the DMA INT bit
 * is sticky, not a hardware event queue. Task delays can span many laps. */
static void build_ring(void) {
 dma_build_ring(cbs, HDMI_DMA_SEGS, bus(cbs), bus(buf), SEG_WORDS * 4u, 0x7e000000u | (HDMI_HD_BASE - PERIPH + HD_MAI_DATA));
 for (uint32_t i = 0; i < HDMI_DMA_SEGS; i++) cbs[i].ti |= DMA_TI_INTEN;
}

/* Register/order facts: Linux rpi-6.6.y vc4_hdmi_audio_startup/prepare,
 * vc4_hdmi_regs.h and vc4_regs.h; independently implemented, see NOTICE.md.
 * RESET readback is undefined, so bit 0 in 0x3029 is not a stuck-reset test.
 * Enable MAI before sending its format; do not replay the shutdown sequence.
 * Packet RAM's global enable is separate from the audio slot enable. */
static void prepare_audio(void) {
 hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH | MAI_CTL_DLATE | MAI_CTL_ERRORE | MAI_CTL_ERRORF);
 hd(HD_MAI_SMP, (clk.smp_n << 8) | clk.smp_m);
 hd(HD_MAI_CTL, hdmi_mai_ctl_run());
 hd(HD_MAI_FMT, hdmi_mai_fmt(RATE));
 hd(HD_MAI_THR, hdmi_mai_thr());
 hc(HC_MAI_CONFIG, MAI_CONFIG_BIT_REVERSE | MAI_CONFIG_FORMAT_REVERSE | 0x3u);
 hc(HC_MAI_CHANNEL_MAP, hdmi_mai_channel_map());
 hc(HC_AUDIO_PACKET_CONFIG, hdmi_audio_packet_config());
 hc(HC_CRP_CFG, hdmi_crp_cfg(clk.n));
 hc(HC_CTS_0, clk.cts);
 hc(HC_CTS_1, clk.cts);
}

static int start_audio_infoframe(void) {
 uint8_t frame[AUDIO_INFOFRAME_BYTES]; uint32_t words[9];
 uint32_t cfg = REG(HDMI_CORE_BASE + HC_RAM_PACKET_CONFIG) | RAM_PACKET_ENABLE;
 uint32_t slot = HC_RAM_PACKET_START + HC_PACKET_STRIDE * PKT_ID_AUDIO_INFO;
 hdmi_audio_infoframe(frame); hdmi_infoframe_words(frame, sizeof frame, words);
 hc(HC_RAM_PACKET_CONFIG, cfg & ~(1u << PKT_ID_AUDIO_INFO));
 for (uint32_t start = micros(); REG(HDMI_CORE_BASE + HC_RAM_PACKET_STATUS) & (1u << PKT_ID_AUDIO_INFO); )
  if (expired(start, 100000)) return -1;
 for (unsigned index = 0; index < 9; index++) hc(slot + 4 * index, words[index]);
 hc(HC_RAM_PACKET_CONFIG, cfg | (1u << PKT_ID_AUDIO_INFO));
 for (uint32_t start = micros(); !(REG(HDMI_CORE_BASE + HC_RAM_PACKET_STATUS) & (1u << PKT_ID_AUDIO_INFO)); )
  if (expired(start, 100000)) return -1;
 return 0;
}

void hdmi_audio_irq(void) {
 if (hw.ch < 1 || !(dma(DMA_CS) & 4u)) return;
 dmaw(DMA_CS, (dma(DMA_CS) & ~0x102u) | 4u); /* W1C INT; preserve ACTIVE/priorities, leave END/error */
 /* CONBLK identifies the block DMA is reading now; only retire its
  * predecessor, which has a full seven segments before reuse. Never use pcm
  * or packed here: the task may be preparing audio when interrupted. */
 uint32_t current = dma_cb_index(dma(DMA_CONBLK), bus(cbs), HDMI_DMA_SEGS);
 uint32_t seg = (current + HDMI_DMA_SEGS - 1u) & (HDMI_DMA_SEGS - 1u);
 if (!ready[seg]) irq_starved++;
 for (uint32_t i = 0; i < SEG_WORDS; i++) buf[seg * SEG_WORDS + i] = silence[i];
 drain();
 ready[seg] = 0;
 pos_idx = current;
 completions++;
}

/* Fallback when the DMA IRQ is not delivered (seen on a board: DMA ran, the
 * IRQ handler never did). Same bookkeeping as hdmi_audio_irq, driven by the
 * sticky INT bit or the DMA position; a real stall changes neither, so the
 * watchdog still fires. IRQs stay masked so the two paths never interleave. */
static void poll_completions(void) {
 uint32_t flags = irq_save();
 if (hw.ch >= 1) {
  uint32_t cs = dma(DMA_CS), current = dma_cb_index(dma(DMA_CONBLK), bus(cbs), HDMI_DMA_SEGS);
  uint32_t n = (current + HDMI_DMA_SEGS - pos_idx) & (HDMI_DMA_SEGS - 1u);
  if (cs & 4u) dmaw(DMA_CS, (cs & ~0x102u) | 4u);
  if (n || (cs & 4u)) {
   uint32_t seg = (current + HDMI_DMA_SEGS - 1u) & (HDMI_DMA_SEGS - 1u);
   if (!n) n = 1;
   if (!ready[seg]) irq_starved++;
   for (uint32_t i = 0; i < SEG_WORDS; i++) buf[seg * SEG_WORDS + i] = silence[i];
   ready[seg] = 0;
   pos_idx = current; completions += n;
  }
 }
 irq_restore(flags);
}

int hdmi_audio_init(void) {
 if (hw.active) return 0;
 audio_hw.report = hdmi_audio_report;
 if (!probed) {
  probed = 1; last_link = link_state();
  audio_diag_line(&diagnostics, timestamp(), "edid_audio=not-read (CEC forwards validated blocks)");
 }
 attempts++; window_attempts++; retry_since = micros();
 retry_delay = window_attempts == 1 ? RETRY_FIRST_US : RETRY_EVERY_US;
 memset(&observed, 0, sizeof observed); memset(&clk, 0, sizeof clk);
 observed.rate = RATE;
 observed.hsm_ctl = REG(CM_HSMCTL); observed.hsm_div = REG(CM_HSMDIV);
 observed.m_ctl = REG(HDMI_HD_BASE + HD_M_CTL);
 observed.scheduler = REG(HDMI_CORE_BASE + HC_SCHEDULER_CONTROL);
 observed.hotplug = REG(HDMI_CORE_BASE + HC_HOTPLUG);
 /* Get clock rate, not framebuffer width or the sink's preferred EDID mode.
  * Capture even an unsuccessful reply and its exact transport/tag failure. */
 const char *pixel_src = "";
 uint32_t pixel[2] = {9, 0};
 observed.pixel_ok = property(0x30002, pixel, 2);
 observed.pixel_id = pixel[0]; observed.pixel_hz = pixel[1];
 observed.mailbox = observed.pixel_ok ? "ok" : property_fail;
 if (iec958_status(RATE, 0, stL) || iec958_status(RATE, 1, stR)) return fail("channel status");
 memcpy(observed.left, stL, sizeof stL); memcpy(observed.right, stR, sizeof stR);
 uint32_t m[1] = {0};
 if (!property(0x60001, m, 1)) return fail("DMA channel mask unavailable");
 hw.ch = -1; for (int c = 1; c <= 6; c++) if (m[0] & (1u << c)) { hw.ch = c; break; }
 if (hw.ch < 0) return fail("no free DMA channel 1-6");
 uint32_t hsm = hdmi_hsm_hz(observed.hsm_ctl, observed.hsm_div);
 sayhex("CM_HSMCTL ", observed.hsm_ctl); sayhex("CM_HSMDIV ", observed.hsm_div);
 if (!hsm || !(observed.hsm_ctl & 0x80u)) return fail("HSM clock stopped or unknown");
 if (!observed.pixel_ok || pixel[0] != 9 || !hdmi_pixel_plausible(pixel[1])) {
  /* The firmware's GET_CLOCK_RATE may read 0 while HDMI runs: try the
   * measured rate (tag 0x30047), then the visible mode's standard timing. */
  pixel_src = "";
  uint32_t meas[2] = {9, 0};
  if (property(0x30047, meas, 2) && meas[0] == 9 && hdmi_pixel_plausible(meas[1])) { pixel[1] = meas[1]; pixel_src = "firmware-measured"; }
  else {
   uint32_t wh[2] = {0, 0};
   if (property(0x40003, wh, 2)) { uint32_t hz = hdmi_mode_pixel_hz(wh[0], wh[1]); if (hz) { pixel[1] = hz; pixel_src = "mode-table"; } }
   observed.mode_w = wh[0]; observed.mode_h = wh[1];
  }
  if (!*pixel_src) return fail("firmware pixel clock unavailable");
  observed.pixel_src = pixel_src; observed.pixel_used = pixel[1];
 }
 if (hdmi_audio_clocks(RATE, pixel[1], hsm, &clk)) return fail("no N/CTS for this mode");
 if (!(observed.m_ctl & 1u)) return fail("HD block not enabled by firmware (is HDMI on?)");
 sayhex("SCHEDULER_CONTROL ", observed.scheduler);
 if (!(observed.scheduler & SCHEDULER_HDMI_ACTIVE)) return fail("DVI mode: set hdmi_drive=2 on the boot card");
 say("pixel clock ", pixel[1], *pixel_src ? " Hz (fallback)" : " Hz (firmware)");
 say("HSM ", hsm, " Hz (from CM)");
 say("N ", clk.n, ""); say("CTS ", clk.cts, ""); say("SMP N ", clk.smp_n, ""); say("SMP M ", clk.smp_m, ""); say("sample clock ", clk.mai_hz, " Hz");
 prepare_audio();
 if (start_audio_infoframe()) {
  hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH);
  return fail("audio infoframe did not start/stop (packet scheduler)");
 }
 sayhex("RAM_PACKET_CONFIG ", REG(HDMI_CORE_BASE + HC_RAM_PACKET_CONFIG));
 sayhex("MAI_CONFIG ", REG(HDMI_CORE_BASE + HC_MAI_CONFIG));
 sayhex("AUDIO_PACKET_CONFIG ", REG(HDMI_CORE_BASE + HC_AUDIO_PACKET_CONFIG));
 sayhex("CRP_CFG ", REG(HDMI_CORE_BASE + HC_CRP_CFG));
 /* Silence in every segment, then the looping DMA ring. */
 mixer_init(&mx);
 for (uint32_t k = 0; k < HDMI_DMA_SEG_FRAMES; k++) pcm[k] = 0;
 iec958_pack(pcm, HDMI_DMA_SEG_FRAMES, 0, stL, stR, silence);
 for (uint32_t i = 0; i < HDMI_DMA_SEGS; i++) {
  for (uint32_t k = 0; k < SEG_WORDS; k++) buf[i * SEG_WORDS + k] = silence[k];
  ready[i] = 1; /* initial silence is intentional */
 }
 irq_starved = 0;
 build_ring();
 drain();
 sayhex("MAI_CTL after write ", REG(HDMI_HD_BASE + HD_MAI_CTL));
 {char l[80]; char a[9],b[9],c[9],d[9]; hex(a,REG(HDMI_HD_BASE+HD_MAI_THR)); hex(b,REG(HDMI_HD_BASE+HD_MAI_FMT)); hex(c,REG(HDMI_HD_BASE+HD_MAI_SMP)); hex(d,REG(HDMI_HD_BASE+HD_M_CTL)); char *p=l; const char *s="AUDIO hdmi: THR/FMT/SMP/M_CTL "; while(*s)*p++=*s++; for(int k=0;k<4;k++){const char *z=k==0?a:k==1?b:k==2?c:d; while(*z)*p++=*z++; *p++=' ';} *p=0; boot_note(l);}
 REG(DMA_ENABLE) |= 1u << hw.ch; barrier();
 dmaw(DMA_CS, 0x80000000u); for (uint32_t s = micros(); dma(DMA_CS) & 0x80000000u; ) if (expired(s, 10000)) break;
 if (dma(DMA_CS) & 0x80000000u) {
  hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH);
  return fail("DMA reset timed out");
 }
 dmaw(DMA_CS, 0x6u); /* clear END and INT */
 dmaw(DMA_DEBUG, 7u);
 dmaw(DMA_CONBLK, bus(cbs));
 started_us = micros(); last_progress_us = started_us;
 completions = 0; last_completed = 0; pos_idx = 0; poll_mode = 0; detect = "none"; segs_played = 0; starved = 0; errors = 0; stalled = 0;
 REG(PERIPH + 0xb210u) = 1u << (16u + (uint32_t)hw.ch); barrier();
 say("DMA channel ", (uint32_t)hw.ch, " (32-bit, burst 2, DREQ 17)");
 sayhex("DMA destination ", cbs[0].dst);
 dmaw(DMA_CS, 1u | (1u << 28) | (8u << 16) | (15u << 20)); /* ACTIVE, wait for outstanding writes, priority 8, panic 15 */
 delay_us(30000);
 uint32_t mc = REG(HDMI_HD_BASE + HD_MAI_CTL), cs = dma(DMA_CS), conblk = dma(DMA_CONBLK);
 sayhex("DMA CS ", dma(DMA_CS)); sayhex("DMA DEBUG ", dma(DMA_DEBUG)); sayhex("MAI_CTL ", mc);
 sayhex("DMA CONBLK ", conblk);
 say("DMA completed segments ", completions, " in 30 ms");
 if (!(cs & 0x100u) && completions) detect = "irq";
 else if (!(cs & 0x100u) && ((cs & 4u) || conblk != bus(cbs))) {
  /* No IRQ, but the hardware is consuming (INT bit set or CONBLK moved): healthy, so poll instead. */
  poll_mode = 1; detect = "poll"; poll_completions(); say("DMA IRQ missing; polling, segments ", completions, "");
 }
 if ((cs & 0x100u) || !completions) {
  REG(PERIPH + 0xb21cu) = 1u << (16u + (uint32_t)hw.ch); barrier();
  dmaw(DMA_CS, 0x80000000u); hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH);
  if (cs & 0x100u) return fail("DMA error");
  return fail(conblk != bus(cbs) || (cs & 4u) ? "DMA advanced but no completion IRQ (check IRQ routing)" : "DMA is not draining (DREQ or MAI not running)");
 }
 hw.active = 1; why = "ok";
 audio_diag_line(&diagnostics, timestamp(), poll_mode ? "dma_detect=poll (IRQ not seen; DMA position advancing)" : "dma_detect=irq");
 audio0.clocked = 0; /* discard the null backend's elapsed-time anchor */
 audio_hw.task = hdmi_audio_task; audio_hw.tone = hdmi_audio_tone;
 boot_note("AUDIO hdmi: DMA draining; HDMI replaces the null backend (silence until /dev/audio or TONE)");
 record_decision(); save_diagnostics();
 return 0;
}
int hdmi_audio_active(void) { return hw.active; }

/* Runs independently of audio_hw.task: the null sink must keep draining while
 * discovery is deferred. Called only at a main-loop boundary, never from IRQ
 * or service_io (delay_us during startup services I/O). */
void hdmi_audio_poll(void) {
 if (!probed) return;
 (void)timestamp();
 save_diagnostics();
 if (!expired(poll_since, 100000u)) return;
 poll_since = micros();
 if (hw.active) { /* DMA stopped completing segments: log it and fall back to the null drain, never wedge */
  uint32_t comp = completions;
  if (audio_stall_check(&stall_watch, micros(), comp, AUDIO_STALL_US)) {
   struct audio_stall_info si = { audio_ring_queued(&audio0.ring), audio_ring_free(&audio0.ring),
    dma_cb_index(dma(DMA_CONBLK), bus(cbs), HDMI_DMA_SEGS), comp, observed.pixel_used, clk.n, clk.cts, observed.pixel_src };
   char sl[AUDIO_LOG_LINE]; audio_stall_line(sl, sizeof sl, &si);
   REG(PERIPH + 0xb21cu) = 1u << (16u + (uint32_t)hw.ch); barrier();
   dmaw(DMA_CS, 0x80000000u);
   hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH);
   hw.active = 0; audio_hw.task = 0; audio_hw.tone = 0; audio0.clocked = 0;
   why = "DMA stalled"; window_attempts = RETRY_WINDOW; /* no retry until a link edge */
   audio_diag_line(&diagnostics, timestamp(), sl); record_decision();
   return;
  }
 } else audio_stall_reset(&stall_watch);
 uint32_t link = link_state();
 if (link != last_link) {
  last_link = link;
  if (hw.active) {
   REG(PERIPH + 0xb21cu) = 1u << (16u + (uint32_t)hw.ch); barrier();
   dmaw(DMA_CS, 0x80000000u);
   hd(HD_MAI_CTL, MAI_CTL_RESET | MAI_CTL_FLUSH);
   hc(HC_RAM_PACKET_CONFIG, REG(HDMI_CORE_BASE + HC_RAM_PACKET_CONFIG) & ~(1u << PKT_ID_AUDIO_INFO));
   hw.active = 0; audio_hw.task = 0; audio_hw.tone = 0; audio0.clocked = 0;
   why = "HDMI hotplug/mode changed";
   observed.hotplug = REG(HDMI_CORE_BASE + HC_HOTPLUG);
   observed.scheduler = REG(HDMI_CORE_BASE + HC_SCHEDULER_CONTROL);
   observed.m_ctl = REG(HDMI_HD_BASE + HD_M_CTL);
   record_decision();
  }
  audio_diag_line(&diagnostics, timestamp(), "HDMI link changed: retry after settling");
  window_attempts = 0; retry_since = micros(); retry_delay = RETRY_FIRST_US;
 }
 if (hw.active || window_attempts >= RETRY_WINDOW || protocol_busy()) return;
 if (!expired(retry_since, retry_delay)) return;
 (void)hdmi_audio_init();
 if (!hw.active && window_attempts >= RETRY_WINDOW)
  audio_diag_line(&diagnostics, timestamp(), "retry limit reached; null drain continues (each new link edge reopens a window)");
}

void hdmi_audio_task(void) {
 if (!hw.active) return;
 if (poll_mode) poll_completions();
 if (mixer_active(&mx)) mixer_pump_dev(&mx, HDMI_DMA_SEG_FRAMES);
 /* Unsigned subtraction retains full laps and tolerates 32-bit wrap. */
 uint32_t completed = completions, adv = completed - last_completed;
 uint32_t now = micros();
 if (adv) { last_progress_us = now; stalled = 0; }
 else if (!stalled && now - last_progress_us >= HDMI_DMA_SEGS * 8000u) {
  starved++; stalled = 1; /* no completion for an entire ring duration */
 }
 segs_played += adv;
 if (dma(DMA_CS) & 0x100u) { errors++; dmaw(DMA_CS, (dma(DMA_CS) & ~6u) | 0x100u); }
 /* Only seven buffers are behind DMA; never refill its current segment.
  * Account for drains missed before those buffers, including their underruns,
  * then prepare the most recent completed segments. fill rechecks hardware
  * ownership after packing, since an IRQ may advance DMA during this task. */
 uint32_t refill = adv < HDMI_DMA_SEGS ? adv : HDMI_DMA_SEGS - 1u;
 for (uint32_t i = refill; i < adv; i++) read_pcm();
 for (uint32_t i = completed - refill; i != completed; i++) fill(i & (HDMI_DMA_SEGS - 1u));
 last_completed = completed;
 drain();
}
int hdmi_audio_tone(uint32_t hz, uint32_t ms) {
 if (!hw.active) return -1;
 if (mixer_tone(&mx, hz, ms, (uint16_t)TONE_VOL) < 0) return -1;
 tones++; return 0;
}
void hdmi_audio_report(char *out, size_t cap) {
 char t[12]; size_t n = 0; while (out[n]) n++;
 if (!hw.active) { const char *a = "backend=null hdmi_unavailable: "; for (; *a && n + 1 < cap;) out[n++] = *a++; for (const char *b = why; *b && n + 1 < cap;) out[n++] = *b++; out[n] = 0; return; }
 uint32_t el = (micros() - started_us) / 1000u; /* ms */
 uint32_t frames = segs_played * HDMI_DMA_SEG_FRAMES;
 uint32_t fps = el ? (frames / el) * 1000u + (frames % el) * 1000u / el : 0;
 const struct { const char *k; uint32_t v; } f[] = {
  {"backend=hdmi segs=", segs_played}, {" drain_fps=", fps}, {" starved=", starved + irq_starved}, {" dma_err=", errors},
  {" ring_underruns=", (uint32_t)audio0.ring.underruns}, {" ring_queued=", audio_ring_queued(&audio0.ring)}, {" tones=", tones}, {" mixer_active=", mixer_active(&mx)}};
 for (unsigned i = 0; i < sizeof f / sizeof *f; i++) {
  for (const char *s = f[i].k; *s && n + 1 < cap;) out[n++] = *s++;
  number(t, f[i].v); for (char *q = t; *q && n + 1 < cap;) out[n++] = *q++;
 }
 char h[9]; hex(h, REG(HDMI_HD_BASE + HD_MAI_CTL)); const char *l = " mai_ctl=0x"; for (; *l && n + 1 < cap;) out[n++] = *l++;
 for (char *q = h; *q && n + 1 < cap;) out[n++] = *q++;
 out[n] = 0;
}
