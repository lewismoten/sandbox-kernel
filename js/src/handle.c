/* SPDX-License-Identifier: MIT; see handle.h. */
#include "handle.h"
#include <stdio.h>
static const char *const kind_names[JSRT_H_KINDS] = {"?", "file", "audio", "input", "serial", "eink", "timer", "game"};
static struct {uint32_t id; int kind; void (*release)(void *); void *user;} table[JSRT_H_MAX];
static unsigned open_count;
static uint32_t generation; /* never reset: a stale id from an earlier app cannot match */
uint32_t jsrt_handle_open(int kind, void (*release)(void *), void *user) {
  if (kind < 1 || kind >= JSRT_H_KINDS) return 0;
  for (unsigned i = 0; i < JSRT_H_MAX; i++) if (!table[i].id) {
    generation = (generation + 1) & 0x0fffffffu;
    if (!generation) generation = 1;
    table[i].id = generation << 4 | i; table[i].kind = kind; table[i].release = release; table[i].user = user;
    open_count++;
    return table[i].id;
  }
  return 0;
}
static int slot_of(uint32_t id, int kind) {
  unsigned i = id & 15u;
  if (!id || i >= JSRT_H_MAX || table[i].id != id || (kind && table[i].kind != kind)) return -1;
  return (int)i;
}
void *jsrt_handle_get(uint32_t id, int kind) {int i = slot_of(id, kind); return i < 0 ? 0 : table[i].user;}
int jsrt_handle_close(uint32_t id) {
  int i = slot_of(id, 0);
  if (i < 0) return -1;
  void (*rel)(void *) = table[i].release; void *user = table[i].user;
  table[i].id = 0; table[i].release = 0; table[i].user = 0; open_count--;
  if (rel) rel(user);
  return 0;
}
unsigned jsrt_handles_open(void) {return open_count;}
unsigned jsrt_handles_release_all(char *summary, size_t cap) {
  unsigned total = open_count, per[JSRT_H_KINDS] = {0};
  for (unsigned i = 0; i < JSRT_H_MAX; i++) if (table[i].id) per[table[i].kind]++;
  for (unsigned i = 0; i < JSRT_H_MAX; i++) if (table[i].id) jsrt_handle_close(table[i].id);
  size_t n = 0;
  if (cap) summary[0] = 0;
  for (int k = 1; k < JSRT_H_KINDS; k++)
    if (per[k] && n < cap) n += (size_t)snprintf(summary + n, cap - n, "%s%s %u", n ? ", " : "", kind_names[k], per[k]);
  return total;
}
