/* SPDX-License-Identifier: MIT; the element tree of dom.h: CSS subset, layout,
 * focus, dirty rectangles and drawing into the 640x480 XRGB8888 surface.
 *
 * CSS subset (element.style.<name>, values as in CSS):
 *   left top width height  px ("12px" or 12); auto
 *   position               static | absolute (left/top are from the parent's padding box)
 *   display                block (children stacked) | flex | none
 *   flexDirection          row | column
 *   gap padding            px, all four sides alike
 *   border                 "<n>px solid <color>"; borderWidth, borderColor
 *   color background backgroundColor  #rgb #rrggbb rgb(r,g,b) or a few names; transparent
 *   outlineColor           the focus ring
 *   fontSize               9px steps of the 5x9 bitmap font (9px, 18px, 27px, 36px)
 *   textAlign              left | center | right
 * Left out on purpose: selectors and style sheets, margins, percentages, z-index
 * (later siblings paint over earlier ones), wrapping flex lines, fonts other than
 * the bitmap one, transparency other than "no background". */
#include "dom.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "font.h"
#include "alloc.h"
static int32_t W = 640, H = 480; /* the screen mode (dom_screen) */
static uint32_t depth = 32;
#define SLOTS 8
struct dom_el *dom_body, *dom_focused;
struct dom_stats dom_stats;
static dom_loader loader;
static void (*game_draw)(void *game, uint32_t *fb, uint32_t stride);
static struct dom_rect dirty[DOM_MAX_DIRTY];
static uint32_t ndirty;
static int need_layout;
static struct {char src[64]; uint8_t px[320 * 200]; uint32_t pal[256]; uint32_t refs, used;} pics[SLOTS];
static uint32_t pic_clock;
static const char *const tags[DOM_TAGS] = {"div", "text", "img", "button", "canvas", "game"};

static int32_t min32(int32_t a, int32_t b) {return a < b ? a : b;}
static int32_t max32(int32_t a, int32_t b) {return a > b ? a : b;}
static int clip(struct dom_rect *r, const struct dom_rect *c) {
  int32_t x0 = max32(r->x, c->x), y0 = max32(r->y, c->y), x1 = min32(r->x + r->w, c->x + c->w), y1 = min32(r->y + r->h, c->y + c->h);
  if (x1 <= x0 || y1 <= y0) return 0;
  r->x = x0; r->y = y0; r->w = x1 - x0; r->h = y1 - y0; return 1;
}
static int touches(const struct dom_rect *a, const struct dom_rect *b) {
  return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}
static void unite(struct dom_rect *a, const struct dom_rect *b) {
  int32_t x1 = max32(a->x + a->w, b->x + b->w), y1 = max32(a->y + a->h, b->y + b->h);
  a->x = min32(a->x, b->x); a->y = min32(a->y, b->y); a->w = x1 - a->x; a->h = y1 - a->y;
}
void dom_damage(const struct dom_rect *in) {
  const struct dom_rect screen = {0, 0, W, H};
  struct dom_rect r = *in;
  if (!clip(&r, &screen)) return;
  for (uint32_t i = 0; i < ndirty; i++)
    if (touches(&dirty[i], &r)) {unite(&r, &dirty[i]); dirty[i] = dirty[--ndirty]; i = (uint32_t)-1;} /* merge, then look again */
  if (ndirty == DOM_MAX_DIRTY) {for (uint32_t i = 1; i < ndirty; i++) unite(&dirty[0], &dirty[i]); ndirty = 1; unite(&dirty[0], &r); return;}
  dirty[ndirty++] = r;
}
void dom_screen(int32_t w, int32_t h, uint32_t d) {
  W = w; H = h; depth = d;
  if (dom_body) {dom_body->st.width = W; dom_body->st.height = H;}
  need_layout = 1; ndirty = 0;
  struct dom_rect all = {0, 0, W, H}; dom_damage(&all);
}
void dom_init(dom_loader load, void (*draw_game)(void *, uint32_t *, uint32_t)) {
  loader = load; game_draw = draw_game;
  dom_body = dom_new(DOM_DIV);
  dom_body->in_tree = 1;
  dom_body->st.width = W; dom_body->st.height = H;
  dom_body->st.has_background = 1; dom_body->st.background = 0;
  need_layout = 1; ndirty = 0;
  struct dom_rect all = {0, 0, W, H}; dom_damage(&all);
}
int dom_tag(const char *name) {
  for (int i = 0; i < DOM_TAGS; i++) if (!strcmp(name, tags[i])) return i;
  if (!strcmp(name, "span") || !strcmp(name, "p") || !strcmp(name, "h1") || !strcmp(name, "label")) return DOM_TEXT;
  if (!strcmp(name, "section") || !strcmp(name, "header") || !strcmp(name, "main")) return DOM_DIV;
  return -1;
}
const char *dom_tag_name(int tag) {return tag >= 0 && tag < DOM_TAGS ? tags[tag] : "div";}
struct dom_el *dom_new(int tag) {
  struct dom_el *e = calloc(1, sizeof *e);
  if (!e) return 0;
  e->tag = (uint8_t)tag; e->img = -1;
  e->st.left = e->st.top = e->st.width = e->st.height = DOM_AUTO;
  e->st.color = 0xffffff; e->st.font = 1; e->st.outline_color = 0xffd400;
  e->focusable = tag == DOM_BUTTON || tag == DOM_GAME;
  if (tag == DOM_BUTTON) {e->st.padding = 4; e->st.border = 1; e->st.border_color = 0x8a96b8; e->st.align = 1;}
  return e;
}
static void unref_pic(struct dom_el *e) {if (e->img >= 0 && pics[e->img].refs) pics[e->img].refs--; e->img = -1;}
static void tree_flag(struct dom_el *e, uint8_t on) {e->in_tree = on; for (struct dom_el *c = e->first; c; c = c->next) tree_flag(c, on);}
static int focused_in(const struct dom_el *e) {for (const struct dom_el *f = dom_focused; f; f = f->parent) if (f == e) return 1; return 0;}
void dom_remove(struct dom_el *c) {
  struct dom_el *p = c->parent;
  if (!p) return;
  if (c->in_tree) {dom_damage(&c->old); need_layout = 1;}
  if (focused_in(c)) dom_focused = 0;
  if (c->prev) c->prev->next = c->next; else p->first = c->next;
  if (c->next) c->next->prev = c->prev; else p->last = c->prev;
  c->parent = c->next = c->prev = 0;
  tree_flag(c, 0);
}
void dom_insert_before(struct dom_el *p, struct dom_el *c, struct dom_el *before) {
  if (c->parent) dom_remove(c);
  if (before && before->parent != p) before = 0;
  c->parent = p; c->next = before; c->prev = before ? before->prev : p->last;
  if (c->prev) c->prev->next = c; else p->first = c;
  if (before) before->prev = c; else p->last = c;
  if (p->in_tree) {tree_flag(c, 1); need_layout = 1; c->old.w = c->old.h = 0;}
}
unsigned dom_depth(const struct dom_el *e) {unsigned n = 0; for (; e->parent; e = e->parent) n++; return n;}
unsigned dom_height(const struct dom_el *e) {unsigned h = 0; for (const struct dom_el *c = e->first; c; c = c->next) {unsigned k = dom_height(c) + 1; if (k > h) h = k;} return h;}
void dom_append(struct dom_el *p, struct dom_el *c) {dom_insert_before(p, c, 0);}
void dom_delete(struct dom_el *e) {
  dom_remove(e);
  while (e->first) dom_remove(e->first);
  unref_pic(e);
  free(e->text); free(e->pixels);
  free(e);
}
void dom_changed(struct dom_el *e) {if (e->in_tree) {need_layout = 1; dom_damage(&e->old);}}
int dom_set_text(struct dom_el *e, const char *s, uint32_t n) {
  if (e->text_n == n && (!n || !memcmp(e->text, s, n))) return 0;
  char *t = n ? malloc(n) : 0;
  if (n && !t) return -1;
  if (n) memcpy(t, s, n);
  free(e->text); e->text = t; e->text_n = n;
  dom_changed(e); return 0;
}
int dom_picture_slots_used(void) {int n = 0; for (int i = 0; i < SLOTS; i++) n += pics[i].src[0] != 0; return n;}
int dom_set_src(struct dom_el *e, const char *src) {
  unref_pic(e); dom_changed(e);
  if (!src[0] || strlen(src) >= sizeof pics[0].src) return src[0] ? -1 : 0;
  int s = -1;
  for (int i = 0; i < SLOTS; i++) if (!strcmp(pics[i].src, src)) s = i;
  if (s < 0) {
    for (int i = 0; i < SLOTS; i++) if (!pics[i].refs && (s < 0 || pics[i].used < pics[s].used)) s = i;
    if (s < 0) return -1;
    static uint8_t pal[768];
    pics[s].src[0] = 0;
    memset(pics[s].px, 0, sizeof pics[s].px);
    if (!loader || loader(src, pics[s].px, pal)) return -1;
    for (int i = 0; i < 256; i++) {
      uint32_t r = pal[i * 3] & 63, g = pal[i * 3 + 1] & 63, b = pal[i * 3 + 2] & 63;
      pics[s].pal[i] = (r << 2 | r >> 4) << 16 | (g << 2 | g >> 4) << 8 | (b << 2 | b >> 4);
    }
    strcpy(pics[s].src, src);
  }
  pics[s].refs++; pics[s].used = ++pic_clock; e->img = s;
  return 0;
}
int dom_set_canvas_size(struct dom_el *e, uint32_t w, uint32_t h) {
  if (w > 960 || h > 540) return -1;
  uint32_t *p = calloc(w && h ? w * h : 1, 4);
  if (!p) return -1;
  free(e->pixels); e->pixels = p; e->pw = w; e->ph = h;
  dom_changed(e); return 0;
}

/* CSS values. */
static int px(const char *v, int32_t *out) {
  if (!strcmp(v, "auto") || !v[0]) {*out = DOM_AUTO; return 0;}
  char *end; long n = strtol(v, &end, 10);
  if (end == v || (*end && strcmp(end, "px"))) return -1;
  if (n < -4096 || n > 4096) return -1;
  *out = (int32_t)n; return 0;
}
static int hex(int c) {return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;}
int dom_parse_color(const char *s, uint32_t *rgb) {
  static const struct {const char *n; uint32_t c;} names[] = {
    {"black", 0}, {"white", 0xffffff}, {"red", 0xff0000}, {"green", 0x008000}, {"blue", 0x0000ff}, {"yellow", 0xffff00},
    {"gray", 0x808080}, {"orange", 0xffa500}, {"purple", 0x800080}, {"navy", 0x000080}, {"teal", 0x008080}};
  size_t n = strlen(s);
  if (s[0] == '#' && (n == 4 || n == 7)) {
    uint32_t c = 0;
    for (size_t i = 1; i < n; i++) {int d = hex(s[i]); if (d < 0) return -1; c = c << (n == 4 ? 8 : 4) | (uint32_t)(n == 4 ? d * 17 : d);}
    *rgb = c; return 0;
  }
  int r, g, b;
  if (sscanf(s, "rgb(%d,%d,%d)", &r, &g, &b) == 3 || sscanf(s, "rgb(%d, %d, %d)", &r, &g, &b) == 3) {
    if ((r | g | b) & ~255) return -1;
    *rgb = (uint32_t)(r << 16 | g << 8 | b); return 0;
  }
  for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) if (!strcmp(s, names[i].n)) {*rgb = names[i].c; return 0;}
  return -1;
}
int dom_set_style(struct dom_el *e, const char *name, const char *v) {
  struct dom_style s = e->st; int32_t n; uint32_t c;
  if (!strcmp(name, "left")) {if (px(v, &s.left)) return -1;}
  else if (!strcmp(name, "top")) {if (px(v, &s.top)) return -1;}
  else if (!strcmp(name, "width")) {if (px(v, &s.width)) return -1;}
  else if (!strcmp(name, "height")) {if (px(v, &s.height)) return -1;}
  else if (!strcmp(name, "gap")) {if (px(v, &n) || n == DOM_AUTO || n < 0) return -1; s.gap = (int16_t)n;}
  else if (!strcmp(name, "padding")) {if (px(v, &n) || n == DOM_AUTO || n < 0) return -1; s.padding = (int16_t)n;}
  else if (!strcmp(name, "borderWidth")) {if (px(v, &n) || n == DOM_AUTO || n < 0 || n > 32) return -1; s.border = (int16_t)n;}
  else if (!strcmp(name, "color")) {if (dom_parse_color(v, &s.color)) return -1;}
  else if (!strcmp(name, "borderColor")) {if (dom_parse_color(v, &s.border_color)) return -1;}
  else if (!strcmp(name, "outlineColor")) {if (dom_parse_color(v, &s.outline_color)) return -1;}
  else if (!strcmp(name, "background") || !strcmp(name, "backgroundColor")) {
    if (!strcmp(v, "transparent") || !strcmp(v, "none") || !v[0]) s.has_background = 0;
    else if (dom_parse_color(v, &s.background)) return -1; else s.has_background = 1;
  } else if (!strcmp(name, "border")) { /* "2px solid #fff", "none" */
    if (!strcmp(v, "none") || !v[0]) s.border = 0;
    else {
      char w[16], style[16], col[32];
      if (sscanf(v, "%15s %15s %31s", w, style, col) != 3 || strcmp(style, "solid") || px(w, &n) || n == DOM_AUTO || n < 0 || n > 32 || dom_parse_color(col, &c)) return -1;
      s.border = (int16_t)n; s.border_color = c;
    }
  } else if (!strcmp(name, "display")) {
    if (!strcmp(v, "block")) s.display = DOM_BLOCK; else if (!strcmp(v, "flex")) s.display = DOM_FLEX; else if (!strcmp(v, "none")) s.display = DOM_NONE; else return -1;
  } else if (!strcmp(name, "flexDirection")) {
    if (!strcmp(v, "row")) s.row = 1; else if (!strcmp(v, "column")) s.row = 0; else return -1;
  } else if (!strcmp(name, "position")) {
    if (!strcmp(v, "absolute")) s.absolute = 1; else if (!strcmp(v, "static") || !strcmp(v, "relative")) s.absolute = 0; else return -1;
  } else if (!strcmp(name, "textAlign")) {
    if (!strcmp(v, "left")) s.align = 0; else if (!strcmp(v, "center")) s.align = 1; else if (!strcmp(v, "right")) s.align = 2; else return -1;
  } else if (!strcmp(name, "fontSize")) {
    if (px(v, &n) || n == DOM_AUTO || n < 9 || n > 36) return -1;
    s.font = (uint8_t)(n / 9);
  } else return -1;
  if (memcmp(&s, &e->st, sizeof s)) {e->st = s; dom_changed(e);}
  return 0;
}
static int color_text(uint32_t c, char *out, uint32_t cap) {return snprintf(out, cap, "#%06x", (unsigned)c);}
static int px_text(int32_t v, char *out, uint32_t cap) {return v == DOM_AUTO ? snprintf(out, cap, "auto") : snprintf(out, cap, "%dpx", (int)v);}
int dom_get_style(const struct dom_el *e, const char *name, char *out, uint32_t cap) {
  const struct dom_style *s = &e->st;
  if (!strcmp(name, "left")) return px_text(s->left, out, cap);
  if (!strcmp(name, "top")) return px_text(s->top, out, cap);
  if (!strcmp(name, "width")) return px_text(s->width, out, cap);
  if (!strcmp(name, "height")) return px_text(s->height, out, cap);
  if (!strcmp(name, "gap")) return px_text(s->gap, out, cap);
  if (!strcmp(name, "padding")) return px_text(s->padding, out, cap);
  if (!strcmp(name, "borderWidth")) return px_text(s->border, out, cap);
  if (!strcmp(name, "color")) return color_text(s->color, out, cap);
  if (!strcmp(name, "borderColor")) return color_text(s->border_color, out, cap);
  if (!strcmp(name, "outlineColor")) return color_text(s->outline_color, out, cap);
  if (!strcmp(name, "background") || !strcmp(name, "backgroundColor")) return s->has_background ? color_text(s->background, out, cap) : snprintf(out, cap, "transparent");
  if (!strcmp(name, "display")) return snprintf(out, cap, "%s", s->display == DOM_FLEX ? "flex" : s->display == DOM_NONE ? "none" : "block");
  if (!strcmp(name, "flexDirection")) return snprintf(out, cap, "%s", s->row ? "row" : "column");
  if (!strcmp(name, "position")) return snprintf(out, cap, "%s", s->absolute ? "absolute" : "static");
  if (!strcmp(name, "textAlign")) return snprintf(out, cap, "%s", s->align == 1 ? "center" : s->align == 2 ? "right" : "left");
  if (!strcmp(name, "fontSize")) return snprintf(out, cap, "%dpx", s->font * 9);
  if (!strcmp(name, "border")) return s->border ? snprintf(out, cap, "%dpx solid #%06x", s->border, (unsigned)s->border_color) : snprintf(out, cap, "none");
  return -1;
}

/* Text: the 5x9 font, 6 px a character, 11 px a line, at a scale. Lines break
   at '\n' and, given a width, at the last space that fits. */
int32_t dom_text_width(const char *s, uint32_t n, int scale) {(void)s; return (int32_t)n * 6 * scale;}
static uint32_t next_line(const char *s, uint32_t n, uint32_t at, int32_t width, int scale, uint32_t *len) {
  uint32_t end = at, space = 0, fit = width > 0 ? (uint32_t)(width / (6 * scale)) : 0xffffffffu;
  if (!fit) fit = 1;
  while (end < n && s[end] != '\n' && end - at < fit) {if (s[end] == ' ') space = end; end++;}
  if (end < n && s[end] != '\n' && space > at) end = space; /* break at the last space */
  *len = end - at;
  if (end < n && (s[end] == '\n' || s[end] == ' ')) end++;
  return end;
}
static void text_size(const struct dom_el *e, int32_t width, int32_t *w, int32_t *h) {
  int sc = e->st.font; int32_t mw = 0, lines = 0; uint32_t len;
  for (uint32_t at = 0; at < e->text_n; lines++) {at = next_line(e->text, e->text_n, at, width, sc, &len); mw = max32(mw, (int32_t)len * 6 * sc);}
  *w = mw; *h = lines * 11 * sc;
}

/* Layout: block children stack down; a flex row puts them side by side, a
   flex column stacks them; absolute children sit at left/top in the padding box. */
static void lay(struct dom_el *e, int32_t x, int32_t y, int32_t avail, int stretch) {
  const struct dom_style *s = &e->st;
  int32_t pad = s->padding + s->border, w = s->width, h = s->height;
  if (w == DOM_AUTO) w = stretch ? avail : -1;
  int32_t cw = (w >= 0 ? w : avail) - 2 * pad, tw = 0, th = 0;
  if (e->text_n) text_size(e, w >= 0 || stretch ? cw : 0, &tw, &th);
  if (e->tag == DOM_IMG && e->img >= 0) {tw = 320; th = 200;}
  if (e->tag == DOM_CANVAS) {tw = (int32_t)e->pw; th = (int32_t)e->ph;}
  if (e->tag == DOM_GAME) {tw = W; th = H;}
  int row = s->display == DOM_FLEX && s->row;
  int32_t cx = x + pad, cy = y + pad + th, ex = 0, ey = 0, n = 0;
  for (struct dom_el *c = e->first; c; c = c->next) {
    if (c->st.display == DOM_NONE) {c->box.w = c->box.h = 0; continue;}
    if (c->st.absolute) continue;
    if (n++) {if (row) cx += s->gap; else cy += s->gap;}
    if (row) {lay(c, cx, cy, max32(0, x + pad + (w >= 0 ? w - 2 * pad : avail) - cx), 0); cx += c->box.w; ey = max32(ey, c->box.h);}
    else {lay(c, cx, cy, cw, 1); cy += c->box.h; ex = max32(ex, c->box.w);}
  }
  if (row) ex = cx - (x + pad); else ey = cy - (y + pad + th);
  if (w < 0) w = max32(tw, ex) + 2 * pad;
  if (h == DOM_AUTO) h = th + ey + 2 * pad;
  e->box.x = x; e->box.y = y; e->box.w = w; e->box.h = h;
  for (struct dom_el *c = e->first; c; c = c->next)
    if (c->st.absolute && c->st.display != DOM_NONE)
      lay(c, x + pad + (c->st.left == DOM_AUTO ? 0 : c->st.left), y + pad + (c->st.top == DOM_AUTO ? 0 : c->st.top), w - 2 * pad, 0);
}
static void moved(struct dom_el *e) {
  for (struct dom_el *c = e->first; c; c = c->next) moved(c);
  if (memcmp(&e->box, &e->old, sizeof e->box)) {dom_damage(&e->old); dom_damage(&e->box);}
}

/* Drawing, clipped to r, into 32-bit (XRGB8888) or 16-bit (RGB565) pixels. */
struct surf {void *p; uint32_t stride; int d16;};
static inline uint16_t c565(uint32_t c) {return (uint16_t)((c >> 8 & 0xf800) | (c >> 5 & 0x07e0) | (c >> 3 & 0x001f));}
static inline void put(const struct surf *s, uint32_t i, uint32_t c) {if (s->d16) ((uint16_t *)s->p)[i] = c565(c); else ((uint32_t *)s->p)[i] = c;}
static void fill(const struct surf *f, struct dom_rect a, const struct dom_rect *r, uint32_t c) {
  if (!clip(&a, r)) return;
  if (f->d16) {uint16_t v = c565(c); for (int32_t y = a.y; y < a.y + a.h; y++) {uint16_t *p = (uint16_t *)f->p + y * f->stride + a.x; for (int32_t i = 0; i < a.w; i++) p[i] = v;}}
  else for (int32_t y = a.y; y < a.y + a.h; y++) {uint32_t *p = (uint32_t *)f->p + y * f->stride + a.x; for (int32_t i = 0; i < a.w; i++) p[i] = c;}
  dom_stats.pixels += (uint32_t)(a.w * a.h);
}
static void frame(const struct surf *f, struct dom_rect b, int32_t t, const struct dom_rect *r, uint32_t c) {
  if (t <= 0) return;
  struct dom_rect s[4] = {{b.x, b.y, b.w, t}, {b.x, b.y + b.h - t, b.w, t}, {b.x, b.y, t, b.h}, {b.x + b.w - t, b.y, t, b.h}};
  for (int i = 0; i < 4; i++) fill(f, s[i], r, c);
}
static void glyphs(const struct surf *f, struct dom_rect lim, int32_t x, int32_t y, uint32_t c, int sc, const char *s, uint32_t n) {
  for (uint32_t i = 0; i < n; i++, x += 6 * sc) {
    unsigned ch = (unsigned char)s[i];
    if (ch < 32 || ch > 126) ch = '?';
    if (x >= lim.x + lim.w || x + 5 * sc <= lim.x) continue;
    const uint8_t *g = gm_font[ch - 32];
    for (int row = 0; row < (int)GM_FONT_H; row++) {
      if (!g[row]) continue;
      for (int col = 0; col < (int)GM_FONT_W; col++) {
        if (!(g[row] >> (GM_FONT_W - 1 - col) & 1)) continue;
        for (int dy = 0; dy < sc; dy++) for (int dx = 0; dx < sc; dx++) {
          int32_t px_ = x + col * sc + dx, py = y + row * sc + dy;
          if (px_ >= lim.x && px_ < lim.x + lim.w && py >= lim.y && py < lim.y + lim.h) put(f, (uint32_t)(py * (int32_t)f->stride + px_), c);
        }
      }
    }
  }
}
static void scaled(const struct surf *f, struct dom_rect to, struct dom_rect lim, const uint8_t *px, const uint32_t *pal, const uint32_t *rgb, uint32_t sw, uint32_t sh) {
  struct dom_rect a = to;
  if (to.w <= 0 || to.h <= 0 || !clip(&a, &lim)) return;
  uint16_t lut[256];
  if (pal && f->d16) for (int i = 0; i < 256; i++) lut[i] = c565(pal[i]);
  uint32_t step = (uint32_t)(((uint64_t)sw << 16) / (uint32_t)to.w);
  for (int32_t y = a.y; y < a.y + a.h; y++) {
    uint32_t sy = (uint32_t)((int64_t)(y - to.y) * sh / to.h), sx = (uint32_t)(a.x - to.x) * step;
    if (f->d16) {
      uint16_t *d = (uint16_t *)f->p + y * f->stride;
      if (pal) {const uint8_t *s = px + sy * sw; for (int32_t x = a.x; x < a.x + a.w; x++, sx += step) d[x] = lut[s[sx >> 16]];}
      else {const uint32_t *s = rgb + sy * sw; for (int32_t x = a.x; x < a.x + a.w; x++, sx += step) d[x] = c565(s[sx >> 16]);}
    } else {
      uint32_t *d = (uint32_t *)f->p + y * f->stride;
      if (pal) {const uint8_t *s = px + sy * sw; for (int32_t x = a.x; x < a.x + a.w; x++, sx += step) d[x] = pal[s[sx >> 16]];}
      else {const uint32_t *s = rgb + sy * sw; for (int32_t x = a.x; x < a.x + a.w; x++, sx += step) d[x] = s[sx >> 16];}
    }
  }
  dom_stats.pixels += (uint32_t)(a.w * a.h);
}
static void paint(const struct dom_el *e, const struct surf *fb, const struct dom_rect *r) {
  if (e->st.display == DOM_NONE || !touches(&e->box, r)) {
    if (e->st.display == DOM_NONE) return;
    for (const struct dom_el *c = e->first; c; c = c->next) if (c->st.absolute) paint(c, fb, r); /* may sit outside */
    return;
  }
  const struct dom_style *s = &e->st;
  struct dom_rect b = e->box, in = {b.x + s->border, b.y + s->border, b.w - 2 * s->border, b.h - 2 * s->border};
  if (e->tag == DOM_GAME && e->game && game_draw && !fb->d16) {game_draw(e->game, fb->p, fb->stride); dom_stats.pixels += (uint32_t)(W * H);}
  if (s->has_background) fill(fb, b, r, s->background);
  frame(fb, b, s->border, r, s->border_color);
  int32_t pad = s->padding + s->border;
  struct dom_rect content = {b.x + pad, b.y + pad, b.w - 2 * pad, b.h - 2 * pad}, lim = content;
  if (e->tag == DOM_IMG && e->img >= 0) scaled(fb, in, *r, pics[e->img].px, pics[e->img].pal, 0, 320, 200);
  if (e->tag == DOM_CANVAS && e->pixels) scaled(fb, content, *r, 0, 0, e->pixels, e->pw, e->ph);
  if (e->text_n && clip(&lim, r)) {
    int sc = s->font; uint32_t len, at = 0; int32_t y = content.y;
    int32_t width = s->width != DOM_AUTO || !e->parent || !(e->parent->st.display == DOM_FLEX && e->parent->st.row) ? content.w : 0;
    while (at < e->text_n) {
      uint32_t st = at; at = next_line(e->text, e->text_n, at, width, sc, &len);
      int32_t lw = (int32_t)len * 6 * sc, x = content.x + (s->align == 1 ? (content.w - lw) / 2 : s->align == 2 ? content.w - lw : 0);
      glyphs(fb, lim, x, y + sc, s->color, sc, e->text + st, len);
      y += 11 * sc;
    }
  }
  for (const struct dom_el *c = e->first; c; c = c->next) paint(c, fb, r);
  if (e == dom_focused) frame(fb, b, 2, r, s->outline_color);
}
static void remember(struct dom_el *e) {e->old = e->box; for (struct dom_el *c = e->first; c; c = c->next) remember(c);}
uint32_t dom_render(void *px, struct dom_rect *out) {
  dom_layout();
  uint32_t n = ndirty; struct surf fb = {px, (uint32_t)W, depth == 16};
  for (uint32_t i = 0; i < n; i++) {paint(dom_body, &fb, &dirty[i]); out[i] = dirty[i];}
  if (n) {dom_stats.paints++; dom_stats.rects += n; remember(dom_body);}
  ndirty = 0;
  return n;
}
int dom_visible(const struct dom_el *e) {
  if (!e->in_tree) return 0;
  for (; e; e = e->parent) if (e->st.display == DOM_NONE) return 0;
  return 1;
}
void dom_focus(struct dom_el *e) {
  if (e == dom_focused) return;
  if (dom_focused) dom_changed(dom_focused);
  dom_focused = e;
  if (e) dom_changed(e);
}
static struct dom_el *best; static int64_t best_score;
static void look(struct dom_el *e, const struct dom_rect *f, int dir) {
  for (struct dom_el *c = e->first; c; c = c->next) {
    if (c->st.display == DOM_NONE) continue;
    if (c->focusable && c != dom_focused) {
      int32_t fx = f->x + f->w / 2, fy = f->y + f->h / 2, cx = c->box.x + c->box.w / 2, cy = c->box.y + c->box.h / 2;
      int32_t along = dir == 0 ? fy - cy : dir == 1 ? cy - fy : dir == 2 ? fx - cx : cx - fx, across = dir < 2 ? cx - fx : cy - fy;
      if (across < 0) across = -across;
      if (along > 0) {int64_t sc = (int64_t)along + 2 * (int64_t)across; if (!best || sc < best_score) {best = c; best_score = sc;}}
    }
    look(c, f, dir);
  }
}
static struct dom_el *first_focusable(struct dom_el *e) {
  for (struct dom_el *c = e->first; c; c = c->next) {
    if (c->st.display == DOM_NONE) continue;
    if (c->focusable) return c;
    struct dom_el *f = first_focusable(c); if (f) return f;
  }
  return 0;
}
void dom_layout(void) {if (need_layout) {lay(dom_body, 0, 0, W, 1); moved(dom_body); need_layout = 0; dom_stats.layouts++;}}
int dom_focus_move(int dir) {
  dom_layout();
  if (!dom_focused || !dom_visible(dom_focused)) {struct dom_el *f = first_focusable(dom_body); if (!f) return 0; dom_focus(f); return 1;}
  best = 0; look(dom_body, &dom_focused->box, dir);
  if (!best) return 0;
  dom_focus(best); return 1;
}
static void canvas_damage(struct dom_el *e) {if (e->in_tree) dom_damage(&e->box);}
void dom_canvas_fill(struct dom_el *e, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb) {
  if (!e->pixels) return;
  struct dom_rect a = {x, y, w, h}, all = {0, 0, (int32_t)e->pw, (int32_t)e->ph};
  if (!clip(&a, &all)) return;
  struct surf f = {e->pixels, e->pw, 0}; fill(&f, a, &all, rgb); dom_stats.pixels -= (uint32_t)(a.w * a.h);
  canvas_damage(e);
}
void dom_canvas_text(struct dom_el *e, int32_t x, int32_t y, uint32_t rgb, int scale, const char *s, uint32_t n) {
  if (!e->pixels) return;
  struct dom_rect all = {0, 0, (int32_t)e->pw, (int32_t)e->ph};
  struct surf f = {e->pixels, e->pw, 0};
  glyphs(&f, all, x, y, rgb, scale < 1 ? 1 : scale, s, n);
  canvas_damage(e);
}
void dom_canvas_image(struct dom_el *e, const struct dom_el *img, int32_t x, int32_t y, int32_t w, int32_t h) {
  if (!e->pixels || !img) return;
  struct dom_rect to = {x, y, w, h}, all = {0, 0, (int32_t)e->pw, (int32_t)e->ph};
  struct surf f = {e->pixels, e->pw, 0};
  if (img->tag == DOM_IMG && img->img >= 0) scaled(&f, to, all, pics[img->img].px, pics[img->img].pal, 0, 320, 200);
  else if (img->tag == DOM_CANVAS && img->pixels) scaled(&f, to, all, 0, 0, img->pixels, img->pw, img->ph);
  canvas_damage(e);
}
