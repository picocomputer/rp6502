/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/modes.h"
#include "term/color.h"
#include "term/font.h"
#include "term/term.h"
#include "sys/com.h"
#include "sys/vga.h"
#include "scanvideo/scanvideo.h"
#include <pico/stdlib.h>
#include <pico/stdio/driver.h>
#include <stdio.h>

// This terminal emulator supports a subset of xterm/ANSI codes.
// It is designed to support 115200 bps without any flow control.
// The logic herein will make more sense if you remember this:

// 1. The screen data doesn't move when scrolling. Instead, the
//    video begins rendering at y_offset and wraps around.
// 2. The screen doesn't fully clear immediately. To keep the UART
//    buffer from overflowing, lines are cleared in a background task
//    and checked as the cursor moves into them.
// 3. When lines wrap, they are marked so that you can backspace and
//    move forward and back as if it's one long virtual line. This
//    greatly simplifies line editor logic.

#define TERM_STD_HEIGHT 30
#define TERM_MAX_HEIGHT 32
#define TERM_CSI_PARAM_MAX_LEN 16
#define TERM_FG_COLOR_INDEX 7
#define TERM_BG_COLOR_INDEX 0

typedef enum
{
    ansi_state_C0,
    ansi_state_Fe,
    ansi_state_SS2,
    ansi_state_SS3,
    ansi_state_CSI,
    ansi_state_CSI_less,
    ansi_state_CSI_equal,
    ansi_state_CSI_greater,
    ansi_state_CSI_question,
    ansi_state_OSC,
    ansi_state_OSC_esc,
} ansi_state_t;

typedef struct
{
    uint8_t font_code;
    uint8_t attributes;
    uint16_t fg_color;
    uint16_t bg_color;
} term_data_t;

typedef struct term_state
{
    uint8_t width;
    uint8_t height;
    uint8_t x;
    uint8_t y;
    uint8_t save_x;
    uint8_t save_y;
    bool save_origin_mode;
    bool line_wrap;
    bool origin_mode;
    bool wrapped[TERM_MAX_HEIGHT];
    bool dirty[TERM_MAX_HEIGHT];
    bool all_clean;
    uint16_t erase_fg_color[TERM_MAX_HEIGHT];
    uint16_t erase_bg_color[TERM_MAX_HEIGHT];
    uint8_t row_idx[TERM_MAX_HEIGHT];
    uint8_t y_offset;
    uint8_t margin_top;
    uint8_t margin_bot;
    bool bold;
    bool blink;
    bool cursor_enabled;
    bool cursor_is_inv;
    uint16_t fg_color;
    uint16_t bg_color;
    uint16_t default_fg_color;
    uint16_t default_bg_color;
    uint16_t cursor_bg_color;
    uint8_t fg_color_index;
    uint8_t bg_color_index;
    term_data_t *mem;
    term_data_t *ptr;
    absolute_time_t timer;
    ansi_state_t ansi_state;
    uint16_t csi_param[TERM_CSI_PARAM_MAX_LEN];
    char csi_separator[TERM_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
} term_state_t;

static term_state_t term_40;
static term_state_t term_80;
static int16_t term_scanline_begin;

// Translate a logical row (0..height-1) into the start of its physical
// cell row. Reads y_offset and row_idx[] without locking; the renderer
// on Core 1 uses the same path. Worst case is one frame of visual tear
// while a region scroll is mid-update — same severity as today's
// y_offset++ race. No memory barrier required.
static inline term_data_t *term_row_ptr(const term_state_t *term, uint8_t y)
{
    uint8_t slot = term->y_offset + y;
    if (slot >= TERM_MAX_HEIGHT)
        slot -= TERM_MAX_HEIGHT;
    return term->mem + (uint32_t)term->row_idx[slot] * term->width;
}

// Make sure you call this any time you change rows.
// It will process any pending screen clears on the row.
static void term_clean_line(term_state_t *term, uint8_t y)
{
    if (!term->dirty[y])
        return;
    term->dirty[y] = false;
    term_data_t *row = term_row_ptr(term, y);
    uint16_t erase_fg_color = term->erase_fg_color[y];
    uint16_t erase_bg_color = term->erase_bg_color[y];
    for (size_t i = 0; i < term->width; i++)
    {
        row[i].font_code = ' ';
        row[i].fg_color = erase_fg_color;
        row[i].bg_color = erase_bg_color;
    }
}

// Set a new cursor position, 0-indexed
static void term_set_cursor_position(term_state_t *term, uint16_t x, uint16_t y)
{
    bool x_off_screen = false;
    if (x == term->width)
    {
        x--;
        x_off_screen = true;
    }
    term->x = x;
    term->y = y;
    term->ptr = term_row_ptr(term, y) + x;
    term_clean_line(term, y);
    if (x_off_screen)
    {
        // ptr parks one column past row end; matches "pending wrap" state.
        term->x++;
        term->ptr++;
    }
}

static void term_clean_task(term_state_t *term)
{
    // Clean only one line per task
    if (term->all_clean)
        return;
    for (size_t i = 0; i < term->height; i++)
        if (term->dirty[i])
        {
            term_clean_line(term, i);
            return;
        }
    term->all_clean = true;
}

static void term_shift_meta_up(term_state_t *term, uint8_t count)
{
    for (uint8_t i = 0; i < count; i++)
    {
        term->wrapped[i] = term->wrapped[i + 1];
        term->dirty[i] = term->dirty[i + 1];
        term->erase_fg_color[i] = term->erase_fg_color[i + 1];
        term->erase_bg_color[i] = term->erase_bg_color[i + 1];
    }
}

static void term_shift_meta_down(term_state_t *term, uint8_t start)
{
    for (int i = start; i > 0; i--)
    {
        term->wrapped[i] = term->wrapped[i - 1];
        term->dirty[i] = term->dirty[i - 1];
        term->erase_fg_color[i] = term->erase_fg_color[i - 1];
        term->erase_bg_color[i] = term->erase_bg_color[i - 1];
    }
}

static void term_mark_rows_erase(term_state_t *term, uint8_t from, uint8_t to)
{
    for (uint8_t i = from; i < to; i++)
    {
        term->wrapped[i] = false;
        term->dirty[i] = true;
        term->erase_fg_color[i] = term->fg_color;
        term->erase_bg_color[i] = term->bg_color;
    }
    term->all_clean = false;
}

static void term_out_FF(term_state_t *term)
{
    term_mark_rows_erase(term, 0, term->height);
    term->x = 0;
    term->y = 0;
    term->y_offset = 0;
    // Rotation is meaningless after a full clear; restore identity so any
    // permutation left by a prior DECSTBM scroll doesn't outlive the wipe.
    for (uint8_t i = 0; i < TERM_MAX_HEIGHT; i++)
        term->row_idx[i] = i;
    term->ptr = term_row_ptr(term, 0);
    term_clean_line(term, 0);
}

static void term_out_RIS(term_state_t *term)
{
    term->ansi_state = ansi_state_C0;
    term->fg_color_index = TERM_FG_COLOR_INDEX;
    term->bg_color_index = TERM_BG_COLOR_INDEX;
    term->default_fg_color = color_256[TERM_FG_COLOR_INDEX];
    term->default_bg_color = color_256[TERM_BG_COLOR_INDEX];
    term->fg_color = color_256[TERM_FG_COLOR_INDEX];
    term->bg_color = color_256[TERM_BG_COLOR_INDEX];
    term->cursor_bg_color = color_256[TERM_FG_COLOR_INDEX];
    term->bold = false;
    term->blink = false;
    term->cursor_enabled = true;
    term->save_x = 0;
    term->save_y = 0;
    term->save_origin_mode = false;
    term->origin_mode = false;
    term->margin_top = 0;
    term->margin_bot = term->height - 1;
    term->x = 0;
    term_out_FF(term);
}

static void term_state_init(term_state_t *term, uint8_t width, term_data_t *mem)
{
    term->width = width;
    term->height = TERM_STD_HEIGHT;
    term->line_wrap = true;
    term->mem = mem;
    term->cursor_is_inv = false;
    term_out_RIS(term);
}

static void term_state_set_height(term_state_t *term, uint8_t height)
{
    assert(height >= 1 && height <= TERM_MAX_HEIGHT);
    while (height != term->height)
    {
        uint8_t logical_row;
        if (height > term->height)
        {
            term->height++;
            if (term->y == term->height - 2)
            {
                // Reveal one row of scrollback above the view; content
                // shifts down by one so the per-screen-row arrays do too.
                term->y++;
                if (!term->y_offset)
                    term->y_offset = TERM_MAX_HEIGHT - 1;
                else
                    term->y_offset--;
                term_shift_meta_down(term, term->height - 1);
                term->wrapped[0] = false;
                term->dirty[0] = false;
                continue;
            }
            // Expose one fresh row at the bottom; reset its metadata so
            // any stale dirty/wrap from a previous shrink can't resurface.
            term->wrapped[term->height - 1] = false;
            term->dirty[term->height - 1] = false;
            logical_row = term->height - 1;
        }
        else
        {
            term->height--;
            if (term->y == term->height)
            {
                term->y--;
                if (++term->y_offset >= TERM_MAX_HEIGHT)
                    term->y_offset -= TERM_MAX_HEIGHT;
                term_shift_meta_up(term, term->height);
                continue;
            }
            logical_row = term->height;
        }
        term_data_t *data = term_row_ptr(term, logical_row);
        for (size_t i = 0; i < term->width; i++)
        {
            data[i].font_code = ' ';
            data[i].fg_color = color_256[TERM_FG_COLOR_INDEX];
            data[i].bg_color = color_256[TERM_BG_COLOR_INDEX];
        }
    }
    // DECSTBM region is meaningless across a height change.
    term->margin_top = 0;
    term->margin_bot = term->height - 1;
}

// Self-inverse swap of one slot: while lit, cursor_bg_color holds the saved
// cell bg; while unlit, it holds the configured cursor color. The cell's fg
// is left untouched, so a glyph under the cursor renders in its own fg on
// the cursor block. Callers must force unlit before touching cell state or
// cursor_bg_color (com_out_chars already does this).
static void term_cursor_set_inv(term_state_t *term, bool inv)
{
    if (!term->cursor_enabled && inv)
        return;
    if (inv == term->cursor_is_inv)
        return;
    term_data_t *term_ptr = term->ptr;
    if (term->x == term->width)
        term_ptr--;
    uint16_t tmp = term_ptr->bg_color;
    term_ptr->bg_color = term->cursor_bg_color;
    term->cursor_bg_color = tmp;
    term->cursor_is_inv = inv;
}

static void sgr_color(term_state_t *term, uint8_t idx, uint16_t *color)
{
    if (idx + 2 < term->csi_param_count &&
        term->csi_param[idx + 1] == 5)
    {
        // e.g. ESC[38;5;255m - Indexed color
        uint16_t color_idx = term->csi_param[idx + 2];
        if (color_idx < 256)
            *color = color_256[color_idx];
    }
    else if (idx + 4 < term->csi_param_count &&
             term->csi_separator[idx] == ';' &&
             term->csi_param[idx + 1] == 2)
    {
        // e.g. ESC[38;2;255;255;255m - RGB color
        *color = SCANVIDEO_ALPHA_MASK |
                 SCANVIDEO_PIXEL_FROM_RGB8(
                     term->csi_param[idx + 2],
                     term->csi_param[idx + 3],
                     term->csi_param[idx + 4]);
    }
    else if (idx + 5 < term->csi_param_count &&
             term->csi_separator[idx] == ':' &&
             term->csi_param[idx + 1] == 2)
    {
        // e.g. ESC[38:2::255:255:255:::m - RGB color (ITU)
        *color = SCANVIDEO_ALPHA_MASK |
                 SCANVIDEO_PIXEL_FROM_RGB8(
                     term->csi_param[idx + 3],
                     term->csi_param[idx + 4],
                     term->csi_param[idx + 5]);
    }
    else if (idx + 1 < term->csi_param_count &&
             term->csi_param[idx + 1] == 1)
    {
        // e.g. ESC[38;1m - transparent
        *color = *color & ~SCANVIDEO_ALPHA_MASK;
    }
}

static void term_out_SGR(term_state_t *term)
{
    for (uint8_t idx = 0; idx < term->csi_param_count; idx++)
    {
        uint16_t param = term->csi_param[idx];
        switch (param)
        {
        case 0: // reset
            term->bold = false;
            term->blink = false;
            term->fg_color_index = TERM_FG_COLOR_INDEX;
            term->bg_color_index = TERM_BG_COLOR_INDEX;
            term->fg_color = term->default_fg_color;
            term->bg_color = term->default_bg_color;
            break;
        case 1: // bold intensity
            term->bold = true;
            term->fg_color = color_256[term->fg_color_index + 8];
            break;
        case 5: // blink (background brightness, IBM VGA quirk)
            term->blink = true;
            term->bg_color = color_256[term->bg_color_index + 8];
            break;
        case 22: // normal intensity
            term->bold = false;
            term->fg_color = color_256[term->fg_color_index];
            break;
        case 25: // not blink
            term->blink = false;
            term->bg_color = color_256[term->bg_color_index];
            break;
        case 30: // foreground color
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            term->fg_color_index = param - 30;
            if (!term->bold)
                term->fg_color = color_256[term->fg_color_index];
            else
                term->fg_color = color_256[term->fg_color_index + 8];
            break;
        case 38:
            sgr_color(term, idx, &term->fg_color);
            return;
        case 39:
            term->fg_color_index = TERM_FG_COLOR_INDEX;
            // When OSC 10 has overridden the default, the override wins and
            // bypasses the bold-bright trick. Without an override, preserve
            // the historical bold-bright-on-default behavior.
            if (term->default_fg_color != color_256[TERM_FG_COLOR_INDEX])
                term->fg_color = term->default_fg_color;
            else if (!term->bold)
                term->fg_color = color_256[TERM_FG_COLOR_INDEX];
            else
                term->fg_color = color_256[TERM_FG_COLOR_INDEX + 8];
            break;
        case 40: // background color
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            term->bg_color_index = param - 40;
            if (!term->blink)
                term->bg_color = color_256[term->bg_color_index];
            else
                term->bg_color = color_256[term->bg_color_index + 8];
            break;
        case 48:
            sgr_color(term, idx, &term->bg_color);
            return;
        case 49:
            term->bg_color_index = TERM_BG_COLOR_INDEX;
            if (term->default_bg_color != color_256[TERM_BG_COLOR_INDEX])
                term->bg_color = term->default_bg_color;
            else if (!term->blink)
                term->bg_color = color_256[TERM_BG_COLOR_INDEX];
            else
                term->bg_color = color_256[TERM_BG_COLOR_INDEX + 8];
            break;
        case 58: // Underline not supported, but eat colors
            return;
        case 90: // bright foreground color
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            term->fg_color_index = param - 90;
            term->fg_color = color_256[term->fg_color_index + 8];
            break;
        case 100: // bright background color
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            term->bg_color_index = param - 100;
            term->bg_color = color_256[term->bg_color_index + 8];
            break;
        }
    }
}

// Save cursor position
static void term_out_SCP(term_state_t *term)
{
    term->save_x = term->x;
    term->save_y = term->y;
    term->save_origin_mode = term->origin_mode;
}

// Restore cursor position
static void term_out_RCP(term_state_t *term)
{
    term->origin_mode = term->save_origin_mode;
    term_set_cursor_position(term, term->save_x, term->save_y);
}

// Only the term currently being rendered should reply to host queries
static bool term_is_visible(term_state_t *term)
{
    int16_t height = vga_canvas_height();
    return (height == 180 || height == 240)
               ? term->width == 40
               : term->width == 80;
}

// Device Status Report
static void term_out_DSR(term_state_t *term)
{
    if (!term_is_visible(term))
        return;
    switch (term->csi_param[0])
    {
    case 5:
        com_in_write_ansi_DSR_ok();
        break;
    case 6:
    {
        unsigned x = term->x;
        unsigned y = term->y;
        if (x == term->width)
            x--;
        if (term->origin_mode)
            y -= term->margin_top;
        com_in_write_ansi_CPR(y + 1, x + 1);
        break;
    }
    }
}

// Primary Device Attributes
static void term_out_DA(term_state_t *term)
{
    if (term_is_visible(term))
        com_in_write_ansi_DA();
}

static void term_out_HT(term_state_t *term)
{
    if (term->x < term->width)
    {
        int xp = 8 - ((term->x + 8) & 7);
        if (term->x + xp > term->width)
            xp = term->width - term->x;
        term->ptr += xp;
        term->x += xp;
    }
}

static void term_out_LF(term_state_t *term, bool wrapping)
{
    if (wrapping)
        term->wrapped[term->y] = true;
    else if (term->y < term->height - 1 && term->wrapped[term->y])
    {
        ++term->y;
        return term_out_LF(term, false);
    }

    if (term->y < term->margin_bot)
    {
        // Above the region's bottom: simply move down.
        ++term->y;
    }
    else if (term->y > term->margin_bot)
    {
        // Below the region: walk down but never scroll; pin at last row.
        if (term->y < term->height - 1)
            ++term->y;
    }
    else
    {
        bool full_screen = (term->margin_top == 0 &&
                            term->margin_bot == term->height - 1);
        if (full_screen)
        {
            // Fast path: rotate the viewport via y_offset.
            if (++term->y_offset == TERM_MAX_HEIGHT)
                term->y_offset = 0;
            term_shift_meta_up(term, term->height - 1);
            term->wrapped[term->height - 1] = false;
            term->dirty[term->height - 1] = false;
            term_data_t *line_ptr = term_row_ptr(term, term->y);
            for (size_t x = 0; x < term->width; x++)
            {
                line_ptr[x].font_code = ' ';
                line_ptr[x].fg_color = term->fg_color;
                line_ptr[x].bg_color = term->bg_color;
            }
        }
        else
        {
            // Region scroll: rotate row_idx[] entries within the region.
            uint8_t top_slot = term->y_offset + term->margin_top;
            if (top_slot >= TERM_MAX_HEIGHT)
                top_slot -= TERM_MAX_HEIGHT;
            uint8_t saved = term->row_idx[top_slot];
            for (uint8_t i = term->margin_top; i < term->margin_bot; i++)
            {
                uint8_t cur = term->y_offset + i;
                if (cur >= TERM_MAX_HEIGHT)
                    cur -= TERM_MAX_HEIGHT;
                uint8_t nxt = term->y_offset + i + 1;
                if (nxt >= TERM_MAX_HEIGHT)
                    nxt -= TERM_MAX_HEIGHT;
                term->row_idx[cur] = term->row_idx[nxt];
            }
            uint8_t bot_slot = term->y_offset + term->margin_bot;
            if (bot_slot >= TERM_MAX_HEIGHT)
                bot_slot -= TERM_MAX_HEIGHT;
            term->row_idx[bot_slot] = saved;

            // Shift logical-row metadata within [margin_top, margin_bot].
            for (uint8_t i = term->margin_top; i < term->margin_bot; i++)
            {
                term->wrapped[i] = term->wrapped[i + 1];
                term->dirty[i] = term->dirty[i + 1];
                term->erase_fg_color[i] = term->erase_fg_color[i + 1];
                term->erase_bg_color[i] = term->erase_bg_color[i + 1];
            }
            term->wrapped[term->margin_bot] = false;
            term->dirty[term->margin_bot] = false;

            term_data_t *line_ptr = term_row_ptr(term, term->margin_bot);
            for (size_t x = 0; x < term->width; x++)
            {
                line_ptr[x].font_code = ' ';
                line_ptr[x].fg_color = term->fg_color;
                line_ptr[x].bg_color = term->bg_color;
            }
        }
    }
    term->ptr = term_row_ptr(term, term->y) + term->x;
    term_clean_line(term, term->y);
}

// Reverse Index (ESC M) — dual of LF. Above margin_top: cursor moves up,
// no scroll. At margin_top: scrolls the region (or whole screen) down by
// one row. Below margin_top inside the region: just moves up.
static void term_out_RI(term_state_t *term)
{
    if (term->y != term->margin_top)
    {
        if (term->y > 0)
            --term->y;
        term->ptr = term_row_ptr(term, term->y) + term->x;
        term_clean_line(term, term->y);
        return;
    }

    bool full_screen = (term->margin_top == 0 &&
                        term->margin_bot == term->height - 1);
    if (full_screen)
    {
        if (term->y_offset == 0)
            term->y_offset = TERM_MAX_HEIGHT - 1;
        else
            --term->y_offset;
        term_shift_meta_down(term, term->height - 1);
        term->wrapped[0] = false;
        term->dirty[0] = false;
    }
    else
    {
        uint8_t bot_slot = term->y_offset + term->margin_bot;
        if (bot_slot >= TERM_MAX_HEIGHT)
            bot_slot -= TERM_MAX_HEIGHT;
        uint8_t saved = term->row_idx[bot_slot];
        for (uint8_t i = term->margin_bot; i > term->margin_top; i--)
        {
            uint8_t cur = term->y_offset + i;
            if (cur >= TERM_MAX_HEIGHT)
                cur -= TERM_MAX_HEIGHT;
            uint8_t prv = term->y_offset + i - 1;
            if (prv >= TERM_MAX_HEIGHT)
                prv -= TERM_MAX_HEIGHT;
            term->row_idx[cur] = term->row_idx[prv];
        }
        uint8_t top_slot = term->y_offset + term->margin_top;
        if (top_slot >= TERM_MAX_HEIGHT)
            top_slot -= TERM_MAX_HEIGHT;
        term->row_idx[top_slot] = saved;

        for (uint8_t i = term->margin_bot; i > term->margin_top; i--)
        {
            term->wrapped[i] = term->wrapped[i - 1];
            term->dirty[i] = term->dirty[i - 1];
            term->erase_fg_color[i] = term->erase_fg_color[i - 1];
            term->erase_bg_color[i] = term->erase_bg_color[i - 1];
        }
        term->wrapped[term->margin_top] = false;
        term->dirty[term->margin_top] = false;
    }
    term_data_t *line_ptr = term_row_ptr(term, term->margin_top);
    for (size_t x = 0; x < term->width; x++)
    {
        line_ptr[x].font_code = ' ';
        line_ptr[x].fg_color = term->fg_color;
        line_ptr[x].bg_color = term->bg_color;
    }
    term->ptr = term_row_ptr(term, term->y) + term->x;
}

static void term_out_CR(term_state_t *term)
{
    term->ptr -= term->x;
    term->x = 0;
}

static void term_out_glyph(term_state_t *term, char ch)
{
    if (term->x == term->width)
    {
        if (term->line_wrap)
        {
            term_out_CR(term);
            term_out_LF(term, true);
        }
        else
        {
            --term->ptr;
            --term->x;
        }
    }
    term->x++;
    term->ptr->font_code = ch;
    term->ptr->fg_color = term->fg_color;
    term->ptr->bg_color = term->bg_color;
    term->ptr++;
}

// Cursor up
static void term_out_CUU(term_state_t *term)
{
    uint16_t rows = term->csi_param[0];
    if (rows < 1)
        rows = 1;
    // Soft fence: cursor already inside the region cannot cross margin_top.
    uint8_t floor = (term->y >= term->margin_top) ? term->margin_top : 0;
    uint16_t y = term->y;
    while (rows && y > floor)
        --rows, --y;
    term->y = y;
    term->ptr = term_row_ptr(term, y) + term->x;
    term_clean_line(term, y);
}

// Cursor down
static void term_out_CUD(term_state_t *term)
{
    uint16_t rows = term->csi_param[0];
    if (rows < 1)
        rows = 1;
    // Soft fence: cursor already inside the region cannot cross margin_bot.
    uint8_t ceil = (term->y <= term->margin_bot) ? term->margin_bot
                                                 : (uint8_t)(term->height - 1);
    uint16_t y = term->y;
    while (rows && y < ceil)
        --rows, ++y;
    term->y = y;
    term->ptr = term_row_ptr(term, y) + term->x;
    term_clean_line(term, y);
}

// Cursor forward
static void term_out_CUF(term_state_t *term)
{
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->width * term->height)
        cols = term->width * term->height;
    if (cols > term->width - term->x)
    {
        if (term->wrapped[term->y])
        {
            term->csi_param[0] = cols - (term->width - term->x);
            term_out_CR(term);
            term_out_LF(term, true);
            return term_out_CUF(term);
        }
        else
            cols = term->width - term->x;
    }
    term->ptr += cols;
    term->x += cols;
}

// Cursor backward
static void term_out_CUB(term_state_t *term)
{
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->width * term->height)
        cols = term->width * term->height;

    if (cols > term->x)
    {
        if (term->y && term->wrapped[term->y - 1])
        {
            term->csi_param[0] = cols - term->x;
            term->y--;
            term->x = term->width;
            term->ptr = term_row_ptr(term, term->y) + term->x;
            return term_out_CUB(term);
        }
        else
            cols = term->x;
    }
    term->ptr -= cols;
    term->x -= cols;
}

// Delete characters
static void term_out_DCH(term_state_t *term)
{
    unsigned max_chars = term->width - term->x;
    for (uint8_t i = term->y; i < term->height - 1 && term->wrapped[i]; i++)
        max_chars += term->width;
    uint16_t chars = term->csi_param[0];
    if (chars < 1)
        chars = 1;
    if (chars > max_chars)
        chars = max_chars;

    // Walk logically: physical rows are not adjacent after a DECSTBM scroll,
    // so refresh row pointers at every row boundary instead of stepping a
    // single ptr through memory.
    uint8_t y_dst = term->y;
    uint8_t x_dst = term->x;
    term_data_t *row_dst = term_row_ptr(term, y_dst);
    uint8_t y_src = term->y;
    uint16_t x_src_raw = (uint16_t)term->x + chars;
    while (x_src_raw >= term->width)
    {
        x_src_raw -= term->width;
        y_src++;
    }
    uint8_t x_src = (uint8_t)x_src_raw;
    term_data_t *row_src = term_row_ptr(term, y_src);

    for (unsigned i = 0; i < max_chars - chars; i++)
    {
        row_dst[x_dst] = row_src[x_src];
        if (++x_dst == term->width)
        {
            x_dst = 0;
            y_dst++;
            row_dst = term_row_ptr(term, y_dst);
        }
        if (++x_src == term->width)
        {
            x_src = 0;
            y_src++;
            row_src = term_row_ptr(term, y_src);
        }
    }
    for (unsigned i = max_chars - chars; i < max_chars; i++)
    {
        row_dst[x_dst].font_code = ' ';
        row_dst[x_dst].fg_color = term->fg_color;
        row_dst[x_dst].bg_color = term->bg_color;
        if (++x_dst == term->width)
        {
            x_dst = 0;
            y_dst++;
            row_dst = term_row_ptr(term, y_dst);
        }
    }

    // Scan back from the end of the logical line to find the real content
    // length, then retire any wrap-chain rows the content no longer reaches.
    unsigned content_len = max_chars;
    uint8_t y_scan = term->y;
    uint16_t x_scan_raw = (uint16_t)term->x + max_chars;
    while (x_scan_raw >= term->width)
    {
        x_scan_raw -= term->width;
        y_scan++;
    }
    uint8_t x_scan = (uint8_t)x_scan_raw;
    term_data_t *row_scan = term_row_ptr(term, y_scan);
    while (content_len > 0)
    {
        if (x_scan == 0)
        {
            y_scan--;
            x_scan = term->width - 1;
            row_scan = term_row_ptr(term, y_scan);
        }
        else
            x_scan--;
        if (row_scan[x_scan].font_code != ' ')
            break;
        content_len--;
    }
    unsigned row_capacity = term->width - term->x;
    for (uint8_t i = term->y; i < term->height - 1 && term->wrapped[i]; i++)
    {
        if (content_len <= row_capacity)
        {
            for (uint8_t j = i; j < term->height - 1 && term->wrapped[j]; j++)
                term->wrapped[j] = false;
            break;
        }
        content_len -= row_capacity;
        row_capacity = term->width;
    }
}

// Set Top and Bottom Margins (CSI Pt ; Pb r)
static void term_out_DECSTBM(term_state_t *term)
{
    uint16_t top = term->csi_param[0];
    uint16_t bot = (term->csi_param_count >= 2) ? term->csi_param[1] : 0;
    if (top < 1)
        top = 1;
    if (bot < 1)
        bot = term->height;
    if (top > term->height)
        top = term->height;
    if (bot > term->height)
        bot = term->height;
    if (top >= bot)
        return; // invalid region: ignore (VT100)

    term->margin_top = (uint8_t)(top - 1);
    term->margin_bot = (uint8_t)(bot - 1);
    // Cursor homes after DECSTBM; origin-relative if DECOM is set.
    if (term->origin_mode)
        term_set_cursor_position(term, 0, term->margin_top);
    else
        term_set_cursor_position(term, 0, 0);
}

// Cursor Position
static void term_out_CUP(term_state_t *term)
{
    // row and col start 1-indexed
    uint16_t row = term->csi_param[0];
    if (row < 1)
        row = 1;

    uint16_t col = term->csi_param[1];
    if (col < 1 || term->csi_param_count < 2)
        col = 1;
    if (col > term->width)
        col = term->width;

    if (term->origin_mode)
    {
        // Row is relative to margin_top; clamp inside the region.
        uint8_t region_h = term->margin_bot - term->margin_top + 1;
        if (row > region_h)
            row = region_h;
        row += term->margin_top;
    }
    else if (row > term->height)
        row = term->height;

    term_set_cursor_position(term, --col, --row);
}

// Erase Line
static void term_out_EL(term_state_t *term)
{
    switch (term->csi_param[0])
    {
    case 0: // to the end of the line
    case 1: // to beginning of the line
    {
        term_data_t *row = term_row_ptr(term, term->y);
        uint16_t erase_fg_color = term->fg_color;
        uint16_t erase_bg_color = term->bg_color;
        uint8_t x, end;
        if (!term->csi_param[0])
        {
            x = term->x;
            end = term->width - 1;
        }
        else
        {
            x = 0;
            end = term->x;
        }
        for (; x <= end; x++)
        {
            row[x].font_code = ' ';
            row[x].fg_color = erase_fg_color;
            row[x].bg_color = erase_bg_color;
        }
        break;
    }
    case 2: // full line
        term->wrapped[term->y] = false;
        term->dirty[term->y] = true;
        term->erase_fg_color[term->y] = term->fg_color;
        term->erase_bg_color[term->y] = term->bg_color;
        term_clean_line(term, term->y);
        break;
    }
}

// Erase Display
static void term_out_ED(term_state_t *term)
{
    switch (term->csi_param[0])
    {
    case 0: //  to end of screen
        term_mark_rows_erase(term, term->y + 1, term->height);
        term_out_EL(term);
        break;
    case 1: //  to beginning of the screen
        term_mark_rows_erase(term, 0, term->y);
        term_out_EL(term);
        break;
    case 2: // full screen
    case 3: // xterm
        term_mark_rows_erase(term, 0, term->height);
        term_clean_line(term, term->y);
        break;
    }
}

static void term_out_state_C0(term_state_t *term, char ch)
{
    switch (ch)
    {
    case '\0': // NUL
    case '\a': // BEL
        break;
    case '\b': // BS
        term->csi_param[0] = 1;
        return term_out_CUB(term);
    case '\t': // HT
        return term_out_HT(term);
    case '\n': // LF
        return term_out_LF(term, false);
    case '\f': // FF
        return term_out_FF(term);
    case '\r': // CR
        return term_out_CR(term);
    case '\33': // ESC
        term->ansi_state = ansi_state_Fe;
        break;
    default:
        return term_out_glyph(term, ch);
    }
}

static void term_out_state_Fe(term_state_t *term, char ch)
{
    if (ch == '[')
    {
        term->ansi_state = ansi_state_CSI;
        term->csi_param_count = 0;
        term->csi_param[0] = 0;
    }
    else if (ch == ']')
    {
        term->ansi_state = ansi_state_OSC;
        term->csi_param_count = 0;
        term->csi_param[0] = 0;
    }
    else if (ch == 'N')
        term->ansi_state = ansi_state_SS2;
    else if (ch == 'O')
        term->ansi_state = ansi_state_SS3;
    else if (ch == 'c')
        term_out_RIS(term);
    else if (ch == 'M')
    {
        term_out_RI(term);
        term->ansi_state = ansi_state_C0;
    }
    else
        term->ansi_state = ansi_state_C0;
}

static void term_out_state_SS2(term_state_t *term, char ch)
{
    (void)ch;
    term->ansi_state = ansi_state_C0;
}

static void term_out_state_SS3(term_state_t *term, char ch)
{
    (void)ch;
    term->ansi_state = ansi_state_C0;
}

static void term_out_CSI(term_state_t *term, char ch)
{
    switch (ch)
    {
    case 'm':
        term_out_SGR(term);
        break;
    case 's':
        term_out_SCP(term);
        break;
    case 'u':
        term_out_RCP(term);
        break;
    case 'n':
        term_out_DSR(term);
        break;
    case 'A':
        term_out_CUU(term);
        break;
    case 'B':
        term_out_CUD(term);
        break;
    case 'C':
        term_out_CUF(term);
        break;
    case 'D':
        term_out_CUB(term);
        break;
    case 'P':
        term_out_DCH(term);
        break;
    case 'H':
        term_out_CUP(term);
        break;
    case 'J':
        term_out_ED(term);
        break;
    case 'K':
        term_out_EL(term);
        break;
    case 'c':
        term_out_DA(term);
        break;
    case 'r':
        term_out_DECSTBM(term);
        break;
    }
}

static void term_out_CSI_question(term_state_t *term, char ch)
{
    switch (ch)
    {
    case 'h': // DECSET
        switch (term->csi_param[0])
        {
        case 6: // DECOM
            term->origin_mode = true;
            term_set_cursor_position(term, 0, term->margin_top);
            break;
        case 12: // AT&T 610
        case 25: // DECTCEM
            term->cursor_enabled = true;
            break;
        }
        break;
    case 'l': // DECRST
        switch (term->csi_param[0])
        {
        case 6: // DECOM
            term->origin_mode = false;
            term_set_cursor_position(term, 0, 0);
            break;
        case 12: // AT&T 610
        case 25: // DECTCEM
            term->cursor_enabled = false;
            break;
        }
        break;
    }
}

static void term_out_state_CSI(term_state_t *term, char ch)
{
    // Drop digits past max, but keep counting params so the terminator still dispatches.
    if (ch >= '0' && ch <= '9')
    {
        if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        {
            term->csi_param[term->csi_param_count] *= 10;
            term->csi_param[term->csi_param_count] += ch - '0';
        }
        return;
    }
    if (ch == ';' || ch == ':')
    {
        if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
            term->csi_separator[term->csi_param_count] = ch;
        if (++term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
            term->csi_param[term->csi_param_count] = 0;
        else
            term->csi_param_count = TERM_CSI_PARAM_MAX_LEN;
        return;
    }
    switch (ch)
    {
    case '<':
        term->ansi_state = ansi_state_CSI_less;
        return;
    case '=':
        term->ansi_state = ansi_state_CSI_equal;
        return;
    case '>':
        term->ansi_state = ansi_state_CSI_greater;
        return;
    case '?':
        term->ansi_state = ansi_state_CSI_question;
        return;
    }
    if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        term->csi_separator[term->csi_param_count] = 0;
    if (++term->csi_param_count > TERM_CSI_PARAM_MAX_LEN)
        term->csi_param_count = TERM_CSI_PARAM_MAX_LEN;
    switch (term->ansi_state)
    {
    case ansi_state_CSI:
        term_out_CSI(term, ch);
        break;
    case ansi_state_CSI_less:
    case ansi_state_CSI_equal:
    case ansi_state_CSI_greater:
        // Private CSI parameter bytes; recognize the sequence so digits don't misparse, then discard.
        break;
    case ansi_state_CSI_question:
        term_out_CSI_question(term, ch);
        break;
    default:
        break;
    }
    term->ansi_state = ansi_state_C0;
}

// Dispatched on OSC terminator (BEL or ST). Implements the dynamic-color
// subset of xterm OSC sequences:
//   OSC 10 ; #rrggbb   set default foreground
//   OSC 11 ; #rrggbb   set default background
//   OSC 12 ; #rrggbb   set cursor color
//   OSC 110            reset default foreground
//   OSC 111            reset default background
//   OSC 112            reset cursor color
// Spec format restricted to "#rrggbb"; other OSC codes/specs are silently
// ignored for forward compatibility.
static void term_out_OSC(term_state_t *term)
{
    uint8_t count = term->csi_param_count;
    bool spec_ok = (count == 8);
    bool empty = (count == 0);
    uint16_t packed = 0;
    if (spec_ok)
        packed = SCANVIDEO_ALPHA_MASK | SCANVIDEO_PIXEL_FROM_RGB8(
                                            term->csi_param[1],
                                            term->csi_param[2],
                                            term->csi_param[3]);
    switch (term->csi_param[0])
    {
    case 10:
        if (spec_ok)
        {
            term->default_fg_color = packed;
            if (term->fg_color_index == TERM_FG_COLOR_INDEX)
                term->fg_color = packed;
        }
        break;
    case 11:
        if (spec_ok)
        {
            term->default_bg_color = packed;
            if (term->bg_color_index == TERM_BG_COLOR_INDEX)
                term->bg_color = packed;
        }
        break;
    case 12:
        if (spec_ok)
            term->cursor_bg_color = packed;
        break;
    case 110:
        if (empty)
        {
            term->default_fg_color = color_256[TERM_FG_COLOR_INDEX];
            if (term->fg_color_index == TERM_FG_COLOR_INDEX)
                term->fg_color = term->bold ? color_256[TERM_FG_COLOR_INDEX + 8]
                                            : color_256[TERM_FG_COLOR_INDEX];
        }
        break;
    case 111:
        if (empty)
        {
            term->default_bg_color = color_256[TERM_BG_COLOR_INDEX];
            if (term->bg_color_index == TERM_BG_COLOR_INDEX)
                term->bg_color = term->blink ? color_256[TERM_BG_COLOR_INDEX + 8]
                                             : color_256[TERM_BG_COLOR_INDEX];
        }
        break;
    case 112:
        if (empty)
            term->cursor_bg_color = color_256[TERM_FG_COLOR_INDEX];
        break;
    }
}

// Streaming OSC body parser; uses csi_param[] as scratch storage.
//   csi_param[0]      = Ps (accumulated digits)
//   csi_param[1..3]   = parsed R, G, B bytes once spec begins
//   csi_param_count   = sub-state cursor:
//       0           collecting Ps digits
//       1           saw ';', expecting '#'
//       2..7        accumulating hex digits of #rrggbb
//       8           spec complete, awaiting terminator
//       0xFF        malformed; drain until terminator
static void term_out_state_OSC(term_state_t *term, char ch)
{
    if (ch == '\a') // BEL terminator
    {
        term_out_OSC(term);
        term->ansi_state = ansi_state_C0;
        return;
    }
    if (ch == '\33') // ESC, possibly start of ST (ESC \)
    {
        term->ansi_state = ansi_state_OSC_esc;
        return;
    }
    if (term->csi_param_count == 0xFF)
        return;
    switch (term->csi_param_count)
    {
    case 0: // collecting Ps digits
        if (ch >= '0' && ch <= '9')
            term->csi_param[0] = term->csi_param[0] * 10 + (ch - '0');
        else if (ch == ';')
        {
            term->csi_param_count = 1;
            term->csi_param[1] = 0;
            term->csi_param[2] = 0;
            term->csi_param[3] = 0;
        }
        else
            term->csi_param_count = 0xFF;
        break;
    case 1: // expect '#'
        if (ch == '#')
            term->csi_param_count = 2;
        else
            term->csi_param_count = 0xFF;
        break;
    case 2:
    case 3:
    case 4:
    case 5:
    case 6:
    case 7:
    {
        uint8_t hex;
        if (ch >= '0' && ch <= '9')
            hex = ch - '0';
        else if (ch >= 'a' && ch <= 'f')
            hex = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F')
            hex = ch - 'A' + 10;
        else
        {
            term->csi_param_count = 0xFF;
            break;
        }
        // States 2,3 -> R; 4,5 -> G; 6,7 -> B
        uint8_t slot = 1 + (term->csi_param_count - 2) / 2;
        term->csi_param[slot] = (term->csi_param[slot] << 4) | hex;
        term->csi_param_count++;
        break;
    }
    default: // spec already complete; any extra chars before terminator are bogus
        term->csi_param_count = 0xFF;
        break;
    }
}

static void term_out_state_OSC_esc(term_state_t *term, char ch)
{
    if (ch == '\\')
        term_out_OSC(term);
    term->ansi_state = ansi_state_C0;
}

static void term_out_char(term_state_t *term, char ch)
{
    if (ch == '\30')
        term->ansi_state = ansi_state_C0;
    else
        switch (term->ansi_state)
        {
        case ansi_state_C0:
            term_out_state_C0(term, ch);
            break;
        case ansi_state_Fe:
            term_out_state_Fe(term, ch);
            break;
        case ansi_state_SS2:
            term_out_state_SS2(term, ch);
            break;
        case ansi_state_SS3:
            term_out_state_SS3(term, ch);
            break;
        case ansi_state_CSI:
        case ansi_state_CSI_less:
        case ansi_state_CSI_equal:
        case ansi_state_CSI_greater:
        case ansi_state_CSI_question:
            term_out_state_CSI(term, ch);
            break;
        case ansi_state_OSC:
            term_out_state_OSC(term, ch);
            break;
        case ansi_state_OSC_esc:
            term_out_state_OSC_esc(term, ch);
            break;
        }
}

static void com_out_chars(const char *buf, int length)
{
    if (length)
    {
        term_cursor_set_inv(&term_40, false);
        term_cursor_set_inv(&term_80, false);
        for (int i = 0; i < length; i++)
        {
            term_out_char(&term_40, buf[i]);
            term_out_char(&term_80, buf[i]);
        }
        term_40.timer = term_80.timer = make_timeout_time_us(2500);
    }
}

void term_init(void)
{
    // prepare console
    static term_data_t term40_mem[40 * TERM_MAX_HEIGHT];
    static term_data_t term80_mem[80 * TERM_MAX_HEIGHT];
    term_state_init(&term_40, 40, term40_mem);
    term_state_init(&term_80, 80, term80_mem);
    // become part of stdout
    static stdio_driver_t term_stdio = {
        .out_chars = com_out_chars,
        .crlf_enabled = true,
    };
    stdio_set_driver_enabled(&term_stdio, true);
}

static void term_blink_cursor(term_state_t *term)
{
    if (time_reached(term->timer))
    {
        term_cursor_set_inv(term, !term->cursor_is_inv);
        // 0.3ms drift to avoid blinking cursor tearing
        if (term->x == term->width)
            // fast blink when off right side
            term->timer = make_timeout_time_us(249700);
        else
            term->timer = make_timeout_time_us(499700);
    }
}

void term_task(void)
{
    term_blink_cursor(&term_40);
    term_blink_cursor(&term_80);
    term_clean_task(&term_40);
    term_clean_task(&term_80);
}

static inline bool __attribute__((optimize("O3")))
term_render_320(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= term_scanline_begin;
    const uint8_t *font_line = &font8[(scanline_id & 7) * 256];
    term_data_t *term_ptr = term_row_ptr(&term_40, scanline_id / 8);
    for (int i = 0; i < 40; i++, term_ptr++)
    {
        uint8_t bits = font_line[term_ptr->font_code];
        uint16_t fg = term_ptr->fg_color;
        uint16_t bg = term_ptr->bg_color;
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    return true;
}

static inline bool __attribute__((optimize("O3")))
term_render_640(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= term_scanline_begin;
    const uint8_t *font_line = &font16[(scanline_id & 15) * 256];
    term_data_t *term_ptr = term_row_ptr(&term_80, scanline_id / 16);
    for (int i = 0; i < 80; i++, term_ptr++)
    {
        uint8_t bits = font_line[term_ptr->font_code];
        uint16_t fg = term_ptr->fg_color;
        uint16_t bg = term_ptr->bg_color;
        modes_render_1bpp(rgb, bits, bg, fg);
        rgb += 8;
    }
    return true;
}

static bool __attribute__((optimize("O3")))
term_render(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    (void)(config_ptr);
    if (width == 320)
        return term_render_320(scanline_id, rgb);
    else
        return term_render_640(scanline_id, rgb);
}

void term_RIS(void)
{
    term_cursor_set_inv(&term_40, false);
    term_cursor_set_inv(&term_80, false);
    term_out_RIS(&term_40);
    term_out_RIS(&term_80);
}

bool term_prog(uint16_t *xregs)
{
    int16_t plane = xregs[2];
    int16_t scanline_begin = xregs[3];
    int16_t scanline_end = xregs[4];
    int16_t height = vga_canvas_height();
    if (!scanline_begin && !scanline_end)
    {
        // Special case to make defaults work with widescreen
        if (height == 180)
            scanline_begin = 2, scanline_end = 178;
        if (height == 360)
            scanline_begin = 4, scanline_end = 356;
    }
    if (!scanline_end)
        scanline_end = height;
    int16_t scanline_count = scanline_end - scanline_begin;
    bool use_40 = height == 180 || height == 240;

    // Check for terminal height is multiple of font height
    if (!scanline_count || scanline_count % (use_40 ? 8 : 16))
        return false;

    // Program the new scanlines
    if (vga_prog_exclusive(plane, scanline_begin, scanline_end, 0, term_render))
    {
        if (use_40)
            term_state_set_height(&term_40, scanline_count / 8);
        else
            term_state_set_height(&term_80, scanline_count / 16);
        term_scanline_begin = scanline_begin;
        return true;
    }
    return false;
}
