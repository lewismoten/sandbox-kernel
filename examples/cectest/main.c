/* SPDX-License-Identifier: MIT
 * Original work (Claude Sonnet 5.5) for Lewis Moten.
 * CECTEST: a test screen for the TV remote. It shows the last received HDMI-CEC frame (who sent it,
 * the opcode and its name, the operands), what the frame means, what the kernel maps it to, this
 * Pi's own CEC addresses and whether it is the active source, and the last 8 distinct events with
 * a count (the same button again bumps its count; "released" folds into the press line).
 * It redraws only when something changed and writes one /dev/log line (so one CEC.TXT line) per new
 * event. Reads the kernel's CEC trace (SBX_IOC_INPUT_CEC); on a kernel without it, it shows the keys
 * the kernel passes on instead. Exit: Esc or Q on a USB keyboard, or hold Select for 3 seconds. */
#include <sandbox/syscall.h>
#define W 960u
#define H 540u
#define NEV 8u
static uint64_t log_fd, fb;
static uint8_t *pix;
static uint32_t pitch, width, height;

/* 5x7 glyphs (own bitmaps): rows top first, 5 bits each, for the characters in GLYPHS in order. */
static const char GLYPHS[] = "0123456789:.->()/=?,+[]#!<ABCDEFGHIJKLMNOPQRSTUVWXYZ";
static const uint8_t FONT[][7] = {
  {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14}, {14,17,1,2,4,8,31}, {31,2,4,2,1,17,14}, {2,6,10,18,31,2,2},
  {31,16,30,1,1,17,14}, {6,8,16,30,17,17,14}, {31,1,2,4,8,8,8}, {14,17,17,14,17,17,14}, {14,17,17,15,1,2,12},
  {0,12,12,0,12,12,0}, {0,0,0,0,0,12,12}, {0,0,0,31,0,0,0}, {16,8,4,2,4,8,16}, {2,4,8,8,8,4,2}, {8,4,2,2,2,4,8},
  {1,2,2,4,8,8,16}, {0,0,31,0,31,0,0}, {14,17,1,2,4,0,4}, {0,0,0,0,12,4,8}, {0,4,4,31,4,4,0}, {14,8,8,8,8,8,14},
  {14,2,2,2,2,2,14}, {10,10,31,10,31,10,10}, {4,4,4,4,4,0,4}, {2,4,8,16,8,4,2},
  {14,17,17,31,17,17,17}, {30,17,17,30,17,17,30}, {14,17,16,16,16,17,14}, {30,17,17,17,17,17,30},
  {31,16,16,30,16,16,31}, {31,16,16,30,16,16,16}, {14,17,16,23,17,17,15}, {17,17,17,31,17,17,17},
  {14,4,4,4,4,4,14}, {7,2,2,2,2,18,12}, {17,18,20,24,20,18,17}, {16,16,16,16,16,16,31},
  {17,27,21,21,17,17,17}, {17,25,21,19,17,17,17}, {14,17,17,17,17,17,14}, {30,17,17,30,16,16,16},
  {14,17,17,17,21,18,13}, {30,17,17,30,20,18,17}, {15,16,16,14,1,1,30}, {31,4,4,4,4,4,4},
  {17,17,17,17,17,17,14}, {17,17,17,17,17,10,4}, {17,17,17,21,21,27,17}, {17,17,10,4,10,17,17},
  {17,17,10,4,4,4,4}, {31,1,2,4,8,16,31},
};
#define CW 6u   /* glyph advance in font pixels */
static void rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint16_t c) {
  for (uint32_t j = y; j < y + h && j < height; j++) {
    uint16_t *row = (uint16_t *)(pix + j * pitch);
    for (uint32_t i = x; i < x + w && i < width; i++) row[i] = c;
  }
}
static void text(uint32_t x, uint32_t y, const char *s, uint32_t sc, uint16_t c) {
  for (; *s; s++, x += CW * sc) {
    char ch = *s;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 32);
    for (uint32_t g = 0; GLYPHS[g]; g++) if (GLYPHS[g] == ch)
      for (uint32_t r = 0; r < 7; r++) for (uint32_t b = 0; b < 5; b++)
        if (FONT[g][r] >> (4 - b) & 1) rect(x + b * sc, y + r * sc, sc, sc, c);
  }
}
/* A small string builder over a fixed buffer. */
typedef struct { char b[96]; uint32_t n; } sb;
static void cat(sb *o, const char *s) { while (*s && o->n < sizeof o->b - 1) o->b[o->n++] = *s++; o->b[o->n] = 0; }
static void dec(sb *o, uint32_t v) { char t[11]; uint32_t k = 0; do { t[k++] = (char)('0' + v % 10); v /= 10; } while (v); while (k) { char c[2] = {t[--k], 0}; cat(o, c); } }
static void hex2(sb *o, uint32_t v) { static const char h[] = "0123456789ABCDEF"; char c[3] = {h[v >> 4 & 15], h[v & 15], 0}; cat(o, c); }
static void hexn(sb *o, uint32_t v) { static const char h[] = "0123456789ABCDEF"; char c[2] = {h[v & 15], 0}; cat(o, c); }

typedef struct { uint32_t code; const char *name; } nm;
static const nm OPS[] = {
  {0x00, "FEATURE ABORT"}, {0x04, "IMAGE VIEW ON"}, {0x0D, "TEXT VIEW ON"}, {0x1A, "GIVE DECK STATUS"}, {0x1B, "DECK STATUS"},
  {0x32, "SET MENU LANGUAGE"}, {0x36, "STANDBY"}, {0x41, "PLAY"}, {0x42, "DECK CONTROL"}, {0x44, "USER CONTROL PRESSED"},
  {0x45, "USER CONTROL RELEASED"}, {0x46, "GIVE OSD NAME"}, {0x47, "SET OSD NAME"}, {0x64, "SET OSD STRING"},
  {0x70, "SYSTEM AUDIO MODE REQUEST"}, {0x72, "SET SYSTEM AUDIO MODE"}, {0x7A, "REPORT AUDIO STATUS"},
  {0x7D, "GIVE SYSTEM AUDIO MODE STATUS"}, {0x7E, "SYSTEM AUDIO MODE STATUS"}, {0x80, "ROUTING CHANGE"},
  {0x81, "ROUTING INFORMATION"}, {0x82, "ACTIVE SOURCE"}, {0x83, "GIVE PHYSICAL ADDRESS"}, {0x84, "REPORT PHYSICAL ADDRESS"},
  {0x85, "REQUEST ACTIVE SOURCE"}, {0x86, "SET STREAM PATH"}, {0x87, "DEVICE VENDOR ID"}, {0x89, "VENDOR COMMAND"},
  {0x8A, "VENDOR REMOTE BUTTON DOWN"}, {0x8B, "VENDOR REMOTE BUTTON UP"}, {0x8C, "GIVE DEVICE VENDOR ID"},
  {0x8D, "MENU REQUEST"}, {0x8E, "MENU STATUS"}, {0x8F, "GIVE POWER STATUS"}, {0x90, "REPORT POWER STATUS"},
  {0x9D, "INACTIVE SOURCE"}, {0x9E, "CEC VERSION"}, {0x9F, "GET CEC VERSION"}, {0xA0, "VENDOR COMMAND WITH ID"}, {0xFF, "ABORT"},
};
static const nm UIS[] = {
  {0x00, "SELECT"}, {0x01, "UP"}, {0x02, "DOWN"}, {0x03, "LEFT"}, {0x04, "RIGHT"}, {0x05, "RIGHT-UP"}, {0x06, "RIGHT-DOWN"},
  {0x07, "LEFT-UP"}, {0x08, "LEFT-DOWN"}, {0x09, "ROOT MENU"}, {0x0A, "SETUP MENU"}, {0x0B, "CONTENTS MENU"},
  {0x0C, "FAVORITE MENU"}, {0x0D, "EXIT"}, {0x20, "0"}, {0x21, "1"}, {0x22, "2"}, {0x23, "3"}, {0x24, "4"}, {0x25, "5"},
  {0x26, "6"}, {0x27, "7"}, {0x28, "8"}, {0x29, "9"}, {0x2A, "DOT"}, {0x2B, "ENTER"}, {0x2C, "CLEAR"}, {0x30, "CHANNEL UP"},
  {0x31, "CHANNEL DOWN"}, {0x32, "PREVIOUS CHANNEL"}, {0x33, "SOUND SELECT"}, {0x34, "INPUT SELECT"}, {0x35, "DISPLAY INFO"},
  {0x36, "HELP"}, {0x37, "PAGE UP"}, {0x38, "PAGE DOWN"}, {0x40, "POWER"}, {0x41, "VOLUME UP"}, {0x42, "VOLUME DOWN"},
  {0x43, "MUTE"}, {0x44, "PLAY"}, {0x45, "STOP"}, {0x46, "PAUSE"}, {0x47, "RECORD"}, {0x48, "REWIND"}, {0x49, "FAST FORWARD"},
  {0x4A, "EJECT"}, {0x4B, "FORWARD"}, {0x4C, "BACKWARD"}, {0x60, "PLAY FUNCTION"}, {0x61, "PAUSE-PLAY FUNCTION"},
  {0x62, "RECORD FUNCTION"}, {0x64, "STOP FUNCTION"}, {0x65, "MUTE FUNCTION"}, {0x66, "RESTORE VOLUME"}, {0x6B, "POWER TOGGLE"},
  {0x6C, "POWER OFF"}, {0x6D, "POWER ON"}, {0x71, "BLUE"}, {0x72, "RED"}, {0x73, "GREEN"}, {0x74, "YELLOW"}, {0x75, "F5"},
};
static const nm KEYS[] = {
  {1, "ESC"}, {14, "BACKSPACE"}, {28, "ENTER"}, {57, "SPACE"}, {59, "F1"}, {60, "F2"}, {61, "F3"}, {62, "F4"}, {102, "HOME"},
  {103, "UP"}, {104, "PAGE UP"}, {105, "LEFT"}, {106, "RIGHT"}, {108, "DOWN"}, {109, "PAGE DOWN"}, {139, "MENU"},
};
static const char *look(const nm *t, uint32_t n, uint32_t code) {
  for (uint32_t i = 0; i < n; i++) if (t[i].code == code) return t[i].name;
  return 0;
}
#define LOOK(t, c) look(t, sizeof t / sizeof t[0], c)
static const char *dev_name(uint32_t a) {
  static const char *const d[16] = {"TV", "REC1", "REC2", "TUNER1", "PLAY1", "AUDIO", "TUNER2", "TUNER3", "PLAY2", "REC3",
                                    "TUNER4", "PLAY3", "R12", "R13", "SPECIAL", "ALL"};
  return d[a & 15];
}
static void keyname(sb *o, uint32_t k) {
  const char *n = LOOK(KEYS, k);
  if (k >= 2 && k <= 11) { cat(o, "KEY "); dec(o, k == 11 ? 0 : k - 1); return; }
  cat(o, "KEY "); dec(o, k); if (n) { cat(o, " "); cat(o, n); }
}

typedef struct { uint8_t len, raw[16]; uint16_t key; uint32_t count, last_ms; uint8_t rel; } ev;
static ev hist[NEV];   /* newest first */
static uint32_t nhist;
static sbx_cec_info cur, last_drawn;
static uint8_t have_trace = 1;
static uint64_t seen_seq;
static uint32_t sel_start, sel_last;

static int same(const ev *e, const uint8_t *raw, uint32_t len) {
  if (e->len != len) return 0;
  for (uint32_t i = 0; i < len; i++) if (e->raw[i] != raw[i]) return 0;
  return 1;
}
static void say(const char *s) { uint32_t n = 0; while (s[n]) n++; sbx_log(log_fd, s, n); }
static void describe(sb *o, const uint8_t *raw, uint32_t len, uint32_t key) {
  if (!len) { keyname(o, key); return; }
  cat(o, "OP "); hex2(o, len > 1 ? raw[1] : 0);
  if (len > 2) { cat(o, " "); for (uint32_t i = 2; i < len && i < 8; i++) hex2(o, raw[i]); }
  if (len > 1 && (raw[1] == 0x44 || raw[1] == 0x45)) {
    if (raw[1] == 0x44 && len > 2) { const char *u = LOOK(UIS, raw[2]); cat(o, " "); cat(o, u ? u : "?"); }
    else if (raw[1] == 0x45) cat(o, " RELEASED");
  } else if (len > 1) { const char *p = LOOK(OPS, raw[1]); if (p) { cat(o, " "); cat(o, p); } }
}
/* Take one received frame (len 0: a kernel key in `key`). Returns 1 for a new or bumped event (logged), 2 when only the screen changes (a release), else 0. */
static int take(const uint8_t *raw, uint32_t len, uint16_t key, uint32_t ms) {
  if (len >= 2 && raw[1] == 0x45) {                    /* release: fold into the newest press line */
    if (nhist && hist[0].len >= 2 && hist[0].raw[1] == 0x44 && !hist[0].rel) { hist[0].rel = 1; return 2; }
    return 0;
  }
  if (len >= 3 && raw[1] == 0x44) {                    /* Select held 3 s exits */
    if (raw[2] == 0) { if (!sel_start || ms - sel_last > 900u) sel_start = ms; sel_last = ms; } else sel_start = 0;
  }
  for (uint32_t i = 0; i < nhist; i++) if (same(&hist[i], raw, len) && hist[i].key == key) {
    ev e = hist[i];
    uint8_t press = len >= 2 && raw[1] == 0x44;
    if (press && i == 0 && !e.rel && ms - e.last_ms < 900u) { hist[0].last_ms = ms; return 0; }   /* the remote's repeat while held */
    for (uint32_t j = i; j > 0; j--) hist[j] = hist[j - 1];
    e.count++; e.rel = 0; e.last_ms = ms; hist[0] = e; return 1;
  }
  if (nhist < NEV) nhist++;
  for (uint32_t j = nhist - 1; j > 0; j--) hist[j] = hist[j - 1];
  hist[0].len = (uint8_t)len; for (uint32_t i = 0; i < len; i++) hist[0].raw[i] = raw[i];
  hist[0].key = key; hist[0].count = 1; hist[0].last_ms = ms; hist[0].rel = 0;
  return 1;
}
static void logev(void) {
  sb o = {{0}, 0}; cat(&o, "CECTEST ");
  if (hist[0].len) { dec(&o, hist[0].raw[0] >> 4); cat(&o, ">"); dec(&o, hist[0].raw[0] & 15); cat(&o, " "); }
  describe(&o, hist[0].raw, hist[0].len, hist[0].key);
  cat(&o, hist[0].key ? " KEY " : " NOT MAPPED"); if (hist[0].key) dec(&o, hist[0].key);
  cat(&o, " X"); dec(&o, hist[0].count);
  say(o.b);
}
static void draw(void) {
  rect(0, 0, width, height, 0x0010);
  uint32_t y = 8;
  text(16, y, have_trace ? "CEC TEST   HOLD SELECT 3 S OR PRESS ESC TO EXIT" : "CEC TEST   PRESS ESC OR Q ON A USB KEYBOARD TO EXIT", 2, 0xAD55); y += 28;
  if (have_trace) { sb o = {{0}, 0};
    cat(&o, "THIS PI: PHYS "); hexn(&o, cur.phys >> 12); cat(&o, "."); hexn(&o, cur.phys >> 8); cat(&o, "."); hexn(&o, cur.phys >> 4); cat(&o, "."); hexn(&o, cur.phys);
    cat(&o, "  LOGICAL "); if (cur.laddr == 15) cat(&o, "NONE"); else { dec(&o, cur.laddr); cat(&o, " "); cat(&o, dev_name(cur.laddr)); }
    text(16, y, o.b, 3, 0xFFFF); y += 28;
    sb p = {{0}, 0};
    cat(&p, "ACTIVE SOURCE: "); cat(&p, cur.active ? "YES" : "NO"); cat(&p, "   ADDRESS: "); cat(&p, cur.state == 1 ? "CLAIMED" : cur.state == 0 ? "CLAIMING" : "NONE");
    text(16, y, p.b, 3, cur.active ? 0x07E0 : 0xFFE0); y += 38; }
  if (!have_trace) { text(16, y, "NO CEC TRACE IN THIS KERNEL: KEYS ONLY", 3, 0xF800); y += 28; }
  if (!nhist) { text(16, y, "WAITING FOR A BUTTON ON THE REMOTE...", 3, 0xFFE0); }
  else {
    const ev *e = &hist[0]; sb a = {{0}, 0};
    cat(&a, "LAST: ");
    if (e->len) { dec(&a, e->raw[0] >> 4); cat(&a, " "); cat(&a, dev_name(e->raw[0] >> 4)); cat(&a, " > "); dec(&a, e->raw[0] & 15); cat(&a, " "); cat(&a, dev_name(e->raw[0] & 15)); }
    else cat(&a, "KERNEL KEY");
    text(16, y, a.b, 3, 0xFFFF); y += 28;
    sb b = {{0}, 0}; cat(&b, "RAW ");
    if (e->len) for (uint32_t i = 0; i < e->len; i++) { hex2(&b, e->raw[i]); cat(&b, " "); } else cat(&b, "-");
    text(16, y, b.b, 3, 0xFFFF); y += 28;
    sb c = {{0}, 0}; cat(&c, "MEANS: ");
    if (e->len >= 2) { const char *p = LOOK(OPS, e->raw[1]); cat(&c, p ? p : "OPCODE ?"); if (e->raw[1] == 0x44 && e->len > 2) { const char *u = LOOK(UIS, e->raw[2]); cat(&c, ": "); cat(&c, u ? u : "?"); cat(&c, " ("); hex2(&c, e->raw[2]); cat(&c, ")"); } }
    else if (e->len == 1) cat(&c, "POLL"); else cat(&c, "-");
    text(16, y, c.b, 3, 0xFFE0); y += 28;
    sb d = {{0}, 0}; cat(&d, "KERNEL MAPS IT TO: ");
    if (e->key) { keyname(&d, e->key); } else cat(&d, "NOT MAPPED");
    text(16, y, d.b, 3, e->key ? 0x07E0 : 0xF800); y += 38;
    for (uint32_t i = 0; i < nhist; i++) {
      sb h = {{0}, 0}; dec(&h, hist[i].count); cat(&h, "X ");
      if (hist[i].len) { dec(&h, hist[i].raw[0] >> 4); cat(&h, ">"); dec(&h, hist[i].raw[0] & 15); cat(&h, " "); }
      describe(&h, hist[i].raw, hist[i].len, hist[i].key);
      if (hist[i].rel) cat(&h, " [REL]");
      cat(&h, hist[i].key ? "" : "  NO KEY");
      text(16, y, h.b, 2 + 1, i == 0 ? 0xFFFF : 0xAD55); y += 26;
    }
  }
}
int main(const sbx_startup *start) {
  if (start->major != 1 || start->minor < 2) return 2;
  if (sbx_open("/dev/log", 8, SBX_O_WRONLY, &log_fd)) return 3;
  sbx_fb_mode mode; void *surface; uint64_t input = 0;
  if (sbx_open("/dev/fb0", 8, SBX_O_RDWR, &fb) || sbx_fb_set_mode(fb, W, H, SBX_FB_RGB565) || sbx_fb_query(fb, &mode) ||
      mode.format != SBX_FB_RGB565 ||
      !mode.width || !mode.height || mode.width > INT32_MAX || mode.height > INT32_MAX ||
      mode.pitch > UINT32_MAX || mode.pitch < mode.width * 2 || mode.buffer_bytes > UINT32_MAX ||
      mode.height > mode.buffer_bytes / mode.pitch || sbx_fb_map(fb, mode.buffer_bytes, &surface)) return 4;
  pix = surface; pitch = (uint32_t)mode.pitch;
  width = (uint32_t)mode.width; height = (uint32_t)mode.height;
  if (sbx_open("/dev/input/event0", 17, SBX_O_RDONLY | SBX_O_NONBLOCK, &input)) return 6;
  say("CECTEST started");
  uint64_t seq = 1, now = 0;
  int dirty = 1, first = 1;
  for (;;) {
    if (sbx_health(start->generation, seq++)) return 5;
    if (sbx_clock_ns(&now)) return 7;
    int32_t st = sbx_cec_get(input, &cur);
    if (st == 0) {
      have_trace = 1;
      if (cur.seq != seen_seq) {
        uint64_t fresh = cur.seq - seen_seq; if (fresh > cur.count) fresh = cur.count;
        if (first) fresh = 0;   /* frames from before this screen started are not new */
        for (uint32_t i = cur.count - (uint32_t)fresh; i < cur.count; i++)
          { int r = take(cur.rec[i].raw, cur.rec[i].len, cur.rec[i].key, cur.rec[i].ms); if (r == 1) logev(); if (r) dirty = 1; }
        seen_seq = cur.seq;
      }
      if (first || cur.laddr != last_drawn.laddr || cur.phys != last_drawn.phys || cur.active != last_drawn.active || cur.state != last_drawn.state) dirty = 1;
      last_drawn = cur; first = 0;
    } else if (have_trace) { have_trace = 0; dirty = 1; first = 0; }
    for (;;) {
      sbx_input_event e; uint64_t got = 0;
      int32_t s2 = sbx_read(input, &e, sizeof e, &got);
      if (s2 == -SBX_EAGAIN) break;
      if (s2 || got != sizeof e) return 6;
      if (e.type != SBX_EV_KEY || e.value != 1) continue;
      if (e.code == SBX_KEY_Q) return 0;
      if (e.code == SBX_KEY_ESC) {   /* a USB keyboard's Esc exits; the remote's Back (also Esc) is a test button */
        if (have_trace && cur.count && cur.rec[cur.count - 1].key == SBX_KEY_ESC && cur.now_ms - cur.rec[cur.count - 1].ms < 1500u) continue;
        return 0;
      }
      if (!have_trace) { if (take(0, 0, (uint16_t)e.code, (uint32_t)(now / 1000000u))) { logev(); dirty = 1; } }
    }
    if (have_trace && sel_start && sel_last - sel_start >= 3000u && cur.count && cur.now_ms - sel_last < 900u) return 0;
    if (dirty) { dirty = 0; draw(); if (sbx_fb_present(fb, 0, 0, width, height)) return 8; }
    if (sbx_sleep_until(now + 50000000u)) return 7;
  }
}
