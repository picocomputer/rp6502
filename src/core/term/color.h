/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_COLOR_H_
#define _CORE_TERM_COLOR_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

extern const uint16_t color_2[2];
extern const uint16_t color_256[256];

/* The console's colors, the console's terminal's own (vterm.c),
 * which OSC 4, 10, 11 and 12 set and a reset sets from color_256: the
 * palette's 256 entries, then the special colors, which are the default
 * foreground and background and the cursor's color. A color is named by
 * one number, an entry of the palette or one of VTERM_COLOR_FG, _BG and
 * _CURSOR, and the special colors are kept apart from the palette, at that
 * number less VTERM_PALETTE. Mode 0 draws the console from them. */
#define VTERM_PALETTE 256
#define VTERM_COLOR_FG 256
#define VTERM_COLOR_BG 257
#define VTERM_COLOR_CURSOR 258
#define VTERM_COLORS 259
#define VTERM_SPECIALS (VTERM_COLORS - VTERM_PALETTE)
extern uint16_t vterm_palette[VTERM_PALETTE];
extern uint16_t vterm_specials[VTERM_SPECIALS];

#endif /* _CORE_TERM_COLOR_H_ */
