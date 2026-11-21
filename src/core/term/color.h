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

/* The console's colors, the console's terminal's own (console_term.c),
 * which OSC 4, 10, 11 and 12 set and a reset sets from color_256: the
 * palette's 256 entries, then the special colors, which are the default
 * foreground and background and the cursor's color. A color is named by
 * one number, an entry of the palette or one of CONSOLE_COLOR_FG, _BG and
 * _CURSOR, and the special colors are kept apart from the palette, at that
 * number less CONSOLE_PALETTE. Mode 0 draws the console from them. */
#define CONSOLE_PALETTE 256
#define CONSOLE_COLOR_FG 256
#define CONSOLE_COLOR_BG 257
#define CONSOLE_COLOR_CURSOR 258
#define CONSOLE_COLORS 259
#define CONSOLE_SPECIALS (CONSOLE_COLORS - CONSOLE_PALETTE)
extern uint16_t console_palette[CONSOLE_PALETTE];
extern uint16_t console_specials[CONSOLE_SPECIALS];

#endif /* _CORE_TERM_COLOR_H_ */
