/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The font arrays exist only to satisfy the Pico renderers term.c carries,
 * which never execute on this machine — the real font is the vid_font ROM
 * on the scanout side. Compiling the real font.c would drag every code
 * page's flash tables into the TCM.
 */

#include "vga/term/font.h"

uint8_t font8[2048];
uint8_t font16[4096];
uint8_t font_dec_8[8 * 32];
uint8_t font_dec_16[16 * 32];
uint8_t italic16[16 * 128];
