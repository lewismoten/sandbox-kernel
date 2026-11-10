/* SPDX-License-Identifier: MIT. Status screen renderer (see render.c). */
#pragma once
#include <stdint.h>
#include <stdbool.h>
void render_starting(uint8_t *plane);
void render_set_tag(const char *name);
void render_status_ex(uint8_t *plane,const char *title,const uint8_t *mark,const char *build_id,const char *usb,const char *error,uint32_t seconds);
void render_page(uint8_t *plane,const uint8_t *bits,bool kernel_mark);
void render_status(uint8_t *plane,const char *build_id,const char *usb,const char *error,uint32_t seconds);
