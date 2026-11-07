/* SPDX-License-Identifier: MIT. Status screen renderer (see render.c). */
#pragma once
#include <stdint.h>
void render_starting(uint8_t *plane);
void render_status(uint8_t *plane,const char *build_id,const char *usb,const char *error,uint32_t seconds);
