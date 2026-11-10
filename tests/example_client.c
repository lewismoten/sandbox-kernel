/* SPDX-License-Identifier: MIT
 * Run the real examples through the SDK HOST_TEST transport. Only transport is
 * faked; geometry, drawing, input handling and exit status are production code.
 */
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#define main example_main
#ifdef EXAMPLE_CLOCK
#include "../examples/clock/main.c"
#else
#include "../examples/hello/main.c"
#endif
#undef main

static uint8_t backing[960 * 540 * 2 + 4096];
static const char *scenario;
static unsigned opens, reads, clocks, presents, healths, sleeps;
static uint32_t actual_w = 960, actual_h = 540, actual_pitch;
static int is(const char *s) { return !strcmp(scenario, s); }
/* ABI pointers to syscall inputs are ARM32. The test's records and their
 * pointed-to local buffers share the host stack's upper bits. mmap's output
 * address is uint64_t and needs no translation. */
static void *local_pointer(void *record, uint64_t address) {
  return (void *)(((uintptr_t)record & ~(uintptr_t)UINT32_MAX) | (uintptr_t)address);
}
int32_t sbx_syscall(uint32_t call, void *record, uint32_t bytes) {
  (void)bytes;
  switch (call) {
  case SBX_CALL_OPEN: {
    sbx_open_rec *r = record;
    if (++opens == 3 && is("inputfail")) return -SBX_ENODEV;
    r->fd = opens;
    return 0;
  }
  case SBX_CALL_WRITE: {
    sbx_io_rec *r = record;
    r->transferred = r->count;
    return 0;
  }
  case SBX_CALL_IOCTL: {
    sbx_ioctl_rec *r = record;
    void *buffer = local_pointer(record, r->buffer);
    if (r->request == SBX_IOC_FB_SET_MODE) {
      sbx_fb_set_rec *m = buffer;
      assert(m->width == 960 && m->height == 540 && m->format == SBX_FB_RGB565);
      return 0;
    }
    if (r->request == SBX_IOC_FB_QUERY) {
      sbx_fb_mode *m = buffer;
      m->width = actual_w; m->height = actual_h; m->pitch = actual_pitch;
      m->format = SBX_FB_RGB565; m->buffer_bytes = actual_pitch * actual_h;
      return 0;
    }
    if (r->request == SBX_IOC_FB_PRESENT) {
      sbx_fb_present_rec *p = buffer;
      assert(p->x == 0 && p->y == 0);
      assert(p->width == actual_w && p->height == actual_h);
      presents++;
      return is("presentfail") ? -SBX_EINVAL : 0;
    }
    return -SBX_ENOTTY;
  }
  case SBX_CALL_MMAP: {
    sbx_mmap_rec *r = record;
    assert(r->length == actual_pitch * actual_h);
    r->address = (uintptr_t)backing;
    return 0;
  }
  case SBX_CALL_CLOCK_GETTIME: {
    clocks++;
    if (is("clockfail") || (is("clocklater") && clocks == 2)) return -SBX_EINVAL;
    ((sbx_clock_rec *)record)->ns = 120000000000ull;
    return 0;
  }
  case SBX_CALL_HEALTH:
    assert(++healths <= 3);  /* catch loops that ignore a failure */
    return 0;
  case SBX_CALL_READ: {
    sbx_io_rec *r = record;
    assert(r->fd == 3);
    reads++;
    if (is("readfail")) return -SBX_EBADF; /* outputs deliberately untouched */
    if (is("shortread")) { r->transferred = 1; return 0; }
    if (reads == 1) return -SBX_EAGAIN;
    sbx_input_event *ev = local_pointer(record, r->buffer);
    *ev = (sbx_input_event){0, SBX_EV_KEY, SBX_KEY_ESC, 1};
    r->transferred = sizeof *ev;
    return 0;
  }
  case SBX_CALL_SLEEP_UNTIL:
    sleeps++;
    return 0;
  default:
    assert(0);
    return -SBX_ENOSYS;
  }
}
int main(int argc, char **argv) {
  assert(argc == 2);
  scenario = argv[1];
  if (is("small") || is("padded")) { actual_w = 640; actual_h = 480; }
  if (is("tiny")) { actual_w = 32; actual_h = 24; }
  actual_pitch = actual_w * 2 + (is("padded") ? 32 : 0);
  memset(backing, 0xaa, sizeof backing);
  sbx_startup start = {40, 1, 7, 1, 0, 0, 0, 16};
  int rc = example_main(&start);
  if (is("inputfail")) { assert(rc == 6 && reads == 0 && clocks == 0); }
  else if (is("readfail") || is("shortread")) { assert(rc == 6 && reads == 1); }
  else if (is("clockfail")) { assert(rc == 7 && clocks == 1 && presents == 0); }
#ifdef EXAMPLE_CLOCK
  else if (is("clocklater")) { assert(rc == 7 && clocks == 2 && presents == 0); }
#endif
  else if (is("presentfail")) { assert(rc == 8 && presents == 1); }
  else { assert(rc == 0 && presents > 0 && reads == 2); }
  /* Catch writes outside the active area, including padding between rows, with
   * full-size backing so the original regression is observable without ASan. */
  for (size_t y = 0; y < actual_h; y++)
    for (size_t x = actual_w * 2; x < actual_pitch; x++)
      assert(backing[y * actual_pitch + x] == 0xaa);
  for (size_t i = actual_pitch * actual_h; i < sizeof backing; i++) assert(backing[i] == 0xaa);
  if (presents) {
    assert(backing[0] != 0xaa); /* an accepted smaller mode actually renders */
    assert(backing[(actual_h - 1) * actual_pitch + (actual_w - 1) * 2] != 0xaa);
  }
  return 0;
}
