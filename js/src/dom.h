/* SPDX-License-Identifier: MIT; a small retained element tree that feels like
 * the DOM: elements with an id, a class, text, a picture or pixels, a CSS
 * subset (dom.c says which), block and flex row/column layout, focus moved by
 * the arrow keys, and redraws limited to what changed (dirty rectangles), at 32 or 16 bits a pixel.
 * Pure C with no JS engine in it: jsrt.c binds it to JavaScript, and the host
 * tests drive it directly. */
#pragma once
#include <stdint.h>
#define DOM_AUTO INT32_MIN
#define DOM_MAX_DIRTY 16u
enum {DOM_DIV, DOM_TEXT, DOM_IMG, DOM_BUTTON, DOM_CANVAS, DOM_GAME, DOM_TAGS};
enum {DOM_BLOCK, DOM_FLEX, DOM_NONE};
struct dom_rect {int32_t x, y, w, h;};
struct dom_style {
  int32_t left, top, width, height;   /* px or DOM_AUTO */
  uint32_t color, background, border_color, outline_color;
  uint8_t has_background, display, row, absolute, font, align; /* font: 1 = 9px text, 2 = 18px ... */
  int16_t gap, padding, border;
};
struct dom_el {
  uint8_t tag, focusable, in_tree;
  struct dom_style st;
  struct dom_el *parent, *first, *last, *next, *prev;
  char *text; uint32_t text_n;
  char id[24], cls[40];
  int img;                            /* picture cache slot, or -1 */
  uint32_t *pixels; uint32_t pw, ph;  /* a canvas's own pixels */
  void *game;                         /* a game element's session (jsrt.c) */
  struct dom_rect box, old;           /* laid out now, and when last drawn */
  void *js;                           /* the binding's object */
};
struct dom_stats {uint32_t layouts, paints, rects, pixels;};
extern struct dom_el *dom_body, *dom_focused;
extern struct dom_stats dom_stats;
/* Pictures: the loader fills a 320 x 200 index picture and 6-bit palette for
   a src (a .GIF, or a game folder's title screen); 0, or -1 when it cannot. */
typedef int (*dom_loader)(const char *src, uint8_t *px, uint8_t *pal);
void dom_init(dom_loader load, void (*draw_game)(void *game, uint32_t *fb, uint32_t stride));
/* The screen's size and depth (32: XRGB8888, 16: RGB565); everything repaints. */
void dom_screen(int32_t w, int32_t h, uint32_t depth);
int dom_tag(const char *name);                  /* DOM_* or -1 */
const char *dom_tag_name(int tag);
struct dom_el *dom_new(int tag);                /* 0 when out of memory */
void dom_delete(struct dom_el *e);              /* unlinks it and its children from it */
void dom_append(struct dom_el *parent, struct dom_el *child);
void dom_insert_before(struct dom_el *parent, struct dom_el *child, struct dom_el *before);
void dom_remove(struct dom_el *child);
int dom_set_text(struct dom_el *e, const char *s, uint32_t n);
int dom_set_src(struct dom_el *e, const char *src);
int dom_set_canvas_size(struct dom_el *e, uint32_t w, uint32_t h);
/* CSS: a camelCase property and its value as text ("12px", "#1d2b53", "flex").
   0, or -1 for a property or value outside the subset. */
int dom_set_style(struct dom_el *e, const char *name, const char *value);
int dom_get_style(const struct dom_el *e, const char *name, char *out, uint32_t cap);
int dom_parse_color(const char *s, uint32_t *rgb);
void dom_changed(struct dom_el *e);             /* layout and paint again */
void dom_damage(const struct dom_rect *r);      /* repaint this part of the screen */
void dom_layout(void);                         /* lays out what changed */
void dom_focus(struct dom_el *e);               /* 0 clears */
/* Moves focus to the nearest focusable element in a direction (0 up, 1 down,
   2 left, 3 right); 1 when it moved. */
int dom_focus_move(int dir);
int dom_visible(const struct dom_el *e);
/* Lays out what changed and paints the damaged parts into fb (640x480);
   returns the rectangles to present (count), copied into out. */
uint32_t dom_render(void *fb, struct dom_rect *out);
/* Canvas drawing, into the element's own pixels (marks it for repaint). */
void dom_canvas_fill(struct dom_el *e, int32_t x, int32_t y, int32_t w, int32_t h, uint32_t rgb);
void dom_canvas_text(struct dom_el *e, int32_t x, int32_t y, uint32_t rgb, int scale, const char *s, uint32_t n);
void dom_canvas_image(struct dom_el *e, const struct dom_el *img, int32_t x, int32_t y, int32_t w, int32_t h);
/* Text width of n characters at a scale (the 5x9 font, 6 px a character). */
int32_t dom_text_width(const char *s, uint32_t n, int scale);
int dom_picture_slots_used(void);
