/* SPDX-License-Identifier: MIT; Copyright (c) 2026 Lewis Moten
 * Original work (Claude Sonnet 5.5). HDMI-CEC on the Zero W: the HDMI core's CEC registers
 * (register layout in include/cec_regs.h, from Linux vc4_hdmi.c facts; NOTICE.md), the display's
 * physical address read from its EDID through the firmware mailbox, keys into /dev/input/event0
 * (core/input.c, the same queue the USB keyboard feeds), and the session log CEC.TXT on the card.
 * Needs config.txt hdmi_ignore_cec=1, or the firmware's own CEC task answers first
 * (scripts/assemble_boot.py). Hardware-untested until a TV session (docs/cec.md). */
#include "kernel.h"
#include "clock.h"
#include "power.h"
#include "cec_drv.h"
#include "cec_edid.h"
#include "hdmi_audio.h"
#include "input.h"
#include "fatput.h"
#include "os.h"

#define CM_HSMCTL (PERIPH + 0x101000u + 0x088u) /* same clock manager words hdmi_audio.c reads */
#define CM_HSMDIV (PERIPH + 0x101000u + 0x08cu)
#define ASSUMED_HSM_HZ 163682000u /* what the 2026-11-09 hardware log read; used only if the registers are unreadable */
#define ASSUMED_PHYS 0x1000u      /* 1.0.0.0, HDMI port 1; used only if the EDID cannot be read */
#define SAVE_FIRST_MS 8000u       /* first CEC.TXT write, then at most one per SAVE_EVERY_MS */
#define SAVE_EVERY_MS 30000u
#define SAVE_MAX 20u

extern bool storage_ready;
bool protocol_busy(void);
static struct cec_drv drv;
static bool up;
static uint32_t saves, last_save_ms;

static uint32_t hrd(void *c, uint32_t o) { (void)c; return REG(HDMI_CORE_BASE + o); }
static void hwr(void *c, uint32_t o, uint32_t v) { (void)c; REG(HDMI_CORE_BASE + o) = v; barrier(); }
static void sink(void *c, uint16_t code, uint8_t value) {
    (void)c;
    if (!input_present()) input_add();           /* a keyboard that left took the device with it */
    struct input_change ch = { EV_KEY, code, value };
    input_frame(&ch, 1);
}
/* milliseconds since the first call, kept in step from the 32-bit microsecond timer (no 64-bit divide;
   called every main-loop pass, far more often than the timer's 71-minute wrap) */
static uint32_t now_ms(void) {
    static uint32_t acc, last, rem; static bool started;
    uint32_t t = micros();
    if (!started) { started = true; last = t; }
    rem += (uint32_t)(t - last); last = t; acc += rem / 1000u; rem %= 1000u;
    return acc;
}

/* firmware property "get EDID block" (tag 0x00030020): block number, status, 128 bytes */
static int edid_block(unsigned blk, uint8_t *out) {
    uint32_t m[40] = { 160u, 0, 0x00030020u, 136u, 0, blk, 0 };
    if (!property_message(m, 40, 200000)) return -1;
    if (!(m[4] & 0x80000000u) || (m[4] & 0x7fffffffu) < 136u || m[6]) return -1;
    for (unsigned i = 0; i < CEC_EDID_BLOCK; i++) out[i] = (uint8_t)(m[7 + i / 4] >> (8 * (i % 4)));
    if (!cec_edid_checksum_ok(out)) return -1;
    hdmi_audio_edid(blk, out);
    return 0;
}
static int find_phys(void) {
    static uint8_t b[CEC_EDID_BLOCK];
    if (edid_block(0, b)) return -1;
    unsigned ext = b[126] > 3 ? 3 : b[126];
    for (unsigned i = 1; i <= ext; i++) {
        if (edid_block(i, b)) continue;
        int p = cec_edid_phys(b);
        if (p >= 0) return p;
    }
    return -1;
}

static void wake(void *ctx) {(void)ctx;power_input(clock_us());}

void cec_hw_init(void) {
    uint32_t hsm = hdmi_hsm_hz(REG(CM_HSMCTL), REG(CM_HSMDIV));
    bool hsm_assumed = !hsm;
    if (hsm_assumed) hsm = ASSUMED_HSM_HZ;
    int p = find_phys();
    drv.rd = hrd; drv.wr = hwr; drv.key = sink; drv.ctx = 0; drv.wake = wake;
    cec_drv_init(&drv, (uint16_t)(p >= 0 ? (unsigned)p : ASSUMED_PHYS), hsm, now_ms());
    if (hsm_assumed) cec_log_line(&drv.log, now_ms(), "HSM clock unreadable: assumed 163682000 Hz");
    cec_log_line(&drv.log, now_ms(), p >= 0 ? "physical address from the EDID" : "EDID unreadable: assumed physical address 1.0.0.0");
    up = true;
    boot_note("CEC started (CEC.TXT after a TV session; docs/cec.md)");
}

static void save_log(uint32_t now) {
    if (!up || !drv.log.dirty || !storage_ready || protocol_busy() || os_running() || saves >= SAVE_MAX) return;
    if (now < SAVE_FIRST_MS || (saves && (uint32_t)(now - last_save_ms) < SAVE_EVERY_MS)) return;
    drv.log.dirty = 0; last_save_ms = now; saves++;
    if (fatsave_put("KERNEL", "CEC.TXT", (const uint8_t *)drv.log.text, drv.log.len)) log_line("CEC.TXT write failed");
}

static void power_task(void);
void cec_hw_task(void) {
    if (!up) return;
    uint32_t now = now_ms();
    power_task();
    cec_drv_task(&drv, now);
    save_log(now);
}

/* A reader that connects late gets the newest CEC lines again (kernel boot_summary). */
void cec_hw_report(void) {
    char t[400], l[104]; uint32_t n;
    if (!up) return;
    n = cec_log_tail(&drv.log, 12, t, sizeof t);
    for (uint32_t i = 0, k = 0; i < n; i++) {
        if (t[i] == '\n') { l[k] = 0; log_line(l); k = 0; }
        else if (k < 100) { if (k == 0) { l[0] = 'C'; l[1] = 'E'; l[2] = 'C'; l[3] = ' '; k = 4; } l[k++] = t[i]; }
    }
}

/* Queue power intent until address claim / queue room; task never blocks. */
static int power_intent;
void cec_hw_power(bool standby) {power_intent=standby?1:2;}
static void power_task(void) {
 if(!power_intent||drv.sm.state!=CEC_SM_CLAIMED) return;
 if(power_intent==1) {
  if(cec_sm_standby(&drv.sm)) return;
 } else {
  if(drv.sm.qn>CEC_SM_QUEUE-2) return;
  cec_sm_announce(&drv.sm);
 }
 power_intent=0;
}
