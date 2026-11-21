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

/* The console's palette, the console's terminal's own (console_term.c): the
 * 256 entries, the default foreground and background, and the cursor's
 * color, which OSC 4, 10, 11 and 12 set and a reset sets from color_256.
 * Mode 0 draws the console from it. */
#define CONSOLE_COLOR_FG 256
#define CONSOLE_COLOR_BG 257
#define CONSOLE_COLOR_CURSOR 258
#define CONSOLE_COLORS 259
extern uint16_t console_palette[CONSOLE_COLORS];

#endif /* _CORE_TERM_COLOR_H_ */
