/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _VGA_TERM_TERM_H_
#define _VGA_TERM_TERM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void term_init(void);
void term_task(void);

void term_RIS(void);
void term_RIS_no_clear(void);
bool term_prog(uint16_t *xregs);

// Scanout state for hosts whose renderer lives outside this file: the
// visible terminal's row bases in cell memory plus the cursor and blink
// state a renderer needs. The rows are the resolved scroll remap, so a
// per-frame snapshot rides through region scrolls and alt-screen swaps.
typedef struct
{
    const void *row[32];
    uint8_t width;
    uint8_t height;
    uint8_t cursor_x, cursor_y, cursor_style;
    bool cursor_enabled, cursor_lit;
    uint16_t cursor_color;
    uint8_t blink_phase;
} term_scanout_t;
void term_scanout(term_scanout_t *out);

#endif /* _VGA_TERM_TERM_H_ */
