/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_TERM_H_
#define _CORE_TERM_TERM_H_

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* Main events
 */

void term_init(void);
void term_task(void);

void term_RIS(void);
void term_RIS_no_clear(void);

/* The model/view seam. term.c is the model — cells, cursor, scroll remap,
 * the ANSI engine; a view renders from it, reading the visible terminal
 * through term_view and term_view_row. mode0.c is the scanvideo view;
 * hosts with their own scanout hardware bring their own.
 */

/* The cell store is the largest thing term.c owns, and two things size
 * it: this switch, and TERM_MAX_HEIGHT in term.c — 32 rows only where
 * the device's 512-line SXGA console exists, 30 everywhere else.
 *
 * TERM_ALT_SCREEN: the ?47 / ?1047 / ?1049 alternate screen buffer.
 *
 * It doubles the cell memory, and on a platform whose cells live in FPGA
 * block memory that is the difference between a feature and a firmware.
 * A fabric that cannot spare the blocks compiles it out and still builds,
 * which is why this is a switch and not a deletion.
 *
 * Off, the escape sequences still parse and the cursor still saves and
 * restores — only the buffer swap is skipped, so a full-screen program
 * draws on the primary and leaves its output behind on exit. That is
 * what a terminal without an alternate screen has always done. */
#ifndef TERM_ALT_SCREEN
#define TERM_ALT_SCREEN 1
#endif

typedef struct
{
    uint8_t font_code;
    uint8_t attributes;
    uint16_t fg_color;
    uint16_t bg_color;
    uint16_t ul_color; // resolved at cell-write: SGR 58 color, or fg if not set
} term_data_t;

_Static_assert(sizeof(term_data_t) == 8, "term_data_t size lock for cell-memory budget");

// Cell `attributes` byte: render-time flags.
#define TERM_ATTR_BLINK_FAST (1u << 0) // SGR 6 rapid (blink phase bit 0)
#define TERM_ATTR_BLINK (1u << 1)      // SGR 5 slow  (blink phase bit 1)
#define TERM_ATTR_UNDERLINE (1u << 2)  // SGR 4
#define TERM_ATTR_DBL_UL (1u << 3)     // SGR 21
#define TERM_ATTR_STRIKE (1u << 4)     // SGR 9
#define TERM_ATTR_OVERLINE (1u << 5)   // SGR 53
#define TERM_ATTR_DEC (1u << 6)        // cell renders from DEC graphics buffer
#define TERM_ATTR_ITALIC (1u << 7)     // SGR 3 (ASCII-only, 8x16 mode only)
// SGR 5 (slow) and SGR 6 (rapid) live at bits 0-1 so they map onto the live
// blink phase counter directly. A cell carries exactly one of the two; the
// renderer ANDs it against the phase so each pulses at its own rate. The two
// underlines sit adjacent at bits 2-3. See TERM_BLINK_TICK_US.
#define TERM_ATTR_ANY_BLINK (TERM_ATTR_BLINK | TERM_ATTR_BLINK_FAST)
#define TERM_ATTR_RENDER_MASK (TERM_ATTR_BLINK | TERM_ATTR_BLINK_FAST |     \
                               TERM_ATTR_UNDERLINE | TERM_ATTR_DBL_UL |     \
                               TERM_ATTR_STRIKE | TERM_ATTR_OVERLINE |      \
                               TERM_ATTR_DEC | TERM_ATTR_ITALIC)

// The visible terminal's cursor and blink state, one call a scanline or a
// frame as the view prefers.
typedef struct
{
    uint8_t width;
    uint8_t height;
    uint8_t cursor_x, cursor_y, cursor_style;
    bool cursor_enabled, cursor_lit;
    uint16_t cursor_color;
    uint8_t blink_phase;
} term_view_t;
void term_view(term_view_t *out);

// The visible terminal's logical row y, through the scroll remap — rides
// region scrolls and alt-screen swaps.
const term_data_t *term_view_row(uint8_t y);

// The view reports its geometry: rows of the 40- or 80-column terminal.
void term_set_height(uint8_t width, uint8_t height);

#endif /* _CORE_TERM_TERM_H_ */
