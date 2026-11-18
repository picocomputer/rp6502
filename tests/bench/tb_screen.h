/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TB_SCREEN_H_
#define _TB_SCREEN_H_

/* The console as mode 0 draws it, read from what cells.c publishes
 * (core/vga/mode/mode0.h), so a test sees what a renderer sees. The grid's
 * width and height are the terminal's own, from the grid showing. */

#include "core/term/cells.h"
#include "core/vga/mode/mode0.h"
#include <stdbool.h>
#include <stdint.h>

/* The cells_task calls that finish what cells.c does a row a tick: the lazy
 * erases of all four screens, and a recolor pass with the one a change
 * during it starts after it, of every row of mode0_cells. */
#define TB_SCREEN_SETTLE (8 * TERM_MAX_HEIGHT + 1)

typedef struct
{
    uint8_t width;
    uint8_t height;
    uint8_t cursor_x, cursor_y, cursor_style;
    bool cursor_enabled, cursor_lit, cursor_pending;
    uint16_t cursor_color;
    uint8_t blink_phase;
    bool reverse;
} tb_screen_t;

static inline void tb_screen(tb_screen_t *v)
{
    const term_grid_t *g = cells_shown(&cells_term);
    const uint32_t cursor = mode0_regs.cursor;
    const uint32_t screen = mode0_regs.screen;
    v->width = g->width;
    v->height = g->height;
    v->cursor_x = MODE0_CURSOR_X(cursor);
    v->cursor_y = MODE0_CURSOR_Y(cursor);
    v->cursor_style = MODE0_CURSOR_STYLE(cursor);
    v->cursor_enabled = (cursor & MODE0_CURSOR_ENABLED) != 0;
    v->cursor_lit = (cursor & MODE0_CURSOR_LIT) != 0;
    /* Nothing draws a pending wrap, so it is the grid's to say. */
    v->cursor_pending = term_pos(g)->wrap_pending;
    v->cursor_color = (uint16_t)mode0_regs.cursor_color;
    v->blink_phase = MODE0_SCREEN_BLINK(screen);
    v->reverse = (screen & MODE0_SCREEN_REVERSE) != 0;
}

static inline const mode0_cell_t *tb_screen_row(uint8_t y)
{
    return &mode0_cells[MODE0_ROW_CELL(mode0_regs.row[y])];
}

/* The colors of the cells of a row, indexed like its cells. */
static inline const mode0_colors_t *tb_screen_row_colors(uint8_t y)
{
    return &mode0_colors[MODE0_ROW_CELL(mode0_regs.row[y])];
}

static inline uint8_t tb_screen_row_size(uint8_t y)
{
    return MODE0_ROW_SIZE(mode0_regs.row[y]);
}

#endif /* _TB_SCREEN_H_ */
