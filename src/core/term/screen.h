/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _CORE_TERM_SCREEN_H_
#define _CORE_TERM_SCREEN_H_

/* One of a grid's two screens, the primary or the alternate: its cursor,
 * its saved cursor and its line sizes. The grid holds what the two share. */

#include <stdbool.h>
#include <stdint.h>

// A cursor's place in a grid.
typedef struct
{
    uint8_t x, y;
    bool wrap_pending; // the last column was written; the next glyph wraps
} term_pos_t;

typedef struct
{
    term_pos_t pos;   // the cursor
    term_pos_t saved; // DECSC's position
    uint8_t *line;    // TERM_LINE_ for each of the grid's rows, the owner's storage
} screen_t;

#endif /* _CORE_TERM_SCREEN_H_ */
