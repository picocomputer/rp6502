/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "term/ansi.h"
#include "term/color.h"
#include "term/font.h"
#include "term/term.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include <stdio.h>

#define TERM_MAX_WIDTH 80
#define TERM_MAX_HEIGHT 32
#define TERM_CSI_PARAM_MAX_LEN 16
#define TERM_LINE_WRAP 1
#define TERM_FG_COLOR_INDEX 7
#define TERM_BG_COLOR_INDEX 0

struct cell_state
{
    uint8_t glyph_code;
    uint8_t attributes;
    uint16_t fg_color;
    uint16_t bg_color;
};

typedef struct term_state
{
    uint8_t width;
    uint8_t height;
    uint8_t x;
    uint8_t y;
    uint8_t y_offset;
    uint16_t fg_color;
    uint16_t bg_color;
    struct cell_state mem[TERM_MAX_WIDTH * TERM_MAX_HEIGHT];
    struct cell_state *ptr;
    absolute_time_t timer;
    int32_t blink_state;
    ansi_state_t ansi_state;
    uint16_t csi_param[TERM_CSI_PARAM_MAX_LEN];
    char csi_separator[TERM_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
} term_state_t;

static term_state_t term_console;

static void term_state_clear(term_state_t *term)
{
    term->x = 0;
    term->y = 0;
    term->y_offset = 0;
    term->ptr = term->mem;
    for (size_t i = 0; i < term->width * term->height; i++)
    {
        term->mem[i].glyph_code = ' ';
        term->mem[i].fg_color = term->fg_color;
        term->mem[i].bg_color = term->bg_color;
    }
}

static void term_state_init(term_state_t *term, uint8_t width, uint8_t height)
{
    term->width = width;
    term->height = height;
    term->fg_color = color256[TERM_FG_COLOR_INDEX];
    term->bg_color = color256[TERM_BG_COLOR_INDEX];
    term->blink_state = 0;
    term->ansi_state = ansi_state_C0;
    term_state_clear(term);
}

static void term_cursor_set_inv(term_state_t *term, bool inv)
{
    if (term->blink_state == -1 || inv == term->blink_state || term->x >= term->width)
        return;
    uint16_t swap = term->ptr->fg_color;
    term->ptr->fg_color = term->ptr->bg_color;
    term->ptr->bg_color = swap;
    term->blink_state = inv;
}

static void sgr_color(term_state_t *term, uint8_t idx, uint16_t *color)
{
    if (idx + 2 < term->csi_param_count &&
        term->csi_param[idx + 1] == 5)
    {
        // e.g. ESC[38;5;255m - Indexed color
        if (color)
        {
            uint16_t color_idx = term->csi_param[idx + 2];
            if (color_idx < 256)
                *color = color256[color_idx];
        }
    }
    else if (idx + 4 < term->csi_param_count &&
             term->csi_separator[idx] == ';' &&
             term->csi_param[idx + 1] == 2)
    {
        // e.g. ESC[38;2;255;255;255m - RBG color
        if (color)
            *color = PICO_SCANVIDEO_ALPHA_MASK |
                     PICO_SCANVIDEO_PIXEL_FROM_RGB8(
                         term->csi_param[idx + 2],
                         term->csi_param[idx + 3],
                         term->csi_param[idx + 4]);
    }
    else if (idx + 5 < term->csi_param_count &&
             term->csi_separator[idx] == ':' &&
             term->csi_param[idx + 1] == 2)
    {
        // e.g. ESC[38:2::255:255:255:::m - RBG color (ITU)
        if (color)
            *color = PICO_SCANVIDEO_ALPHA_MASK |
                     PICO_SCANVIDEO_PIXEL_FROM_RGB8(
                         term->csi_param[idx + 3],
                         term->csi_param[idx + 4],
                         term->csi_param[idx + 5]);
    }
    else if (idx + 1 < term->csi_param_count &&
             term->csi_param[idx + 1] == 1)
    {
        // e.g. ESC[38;1m - transparent
        if (color)
            *color = *color & ~PICO_SCANVIDEO_ALPHA_MASK;
    }
}

static void term_out_sgr(term_state_t *term)
{
    if (term->csi_param_count > TERM_CSI_PARAM_MAX_LEN)
        return;
    for (uint8_t idx = 0; idx < term->csi_param_count; idx++)
    {
        uint16_t param = term->csi_param[idx];
        switch (param)
        {
        case 0: // reset
            term->fg_color = color256[TERM_FG_COLOR_INDEX];
            term->bg_color = color256[TERM_BG_COLOR_INDEX];
            break;
        case 1: // bold intensity
            for (int i = 0; i < 8; i++)
                if (term->fg_color == color256[i])
                    term->fg_color = color256[i + 8];
            break;
        case 22: // normal intensity
            for (int i = 8; i < 16; i++)
                if (term->fg_color == color256[i])
                    term->fg_color = color256[i - 8];
            break;
        case 30: // foreground color
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            term->fg_color = color256[param - 30];
            break;
        case 38:
            sgr_color(term, idx, &term->fg_color);
            return;
        case 39:
            term->fg_color = color256[TERM_FG_COLOR_INDEX];
            break;
        case 40: // background color
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            term->bg_color = color256[param - 40];
            break;
        case 48:
            sgr_color(term, idx, &term->bg_color);
            return;
        case 49:
            term->bg_color = color256[TERM_BG_COLOR_INDEX];
            break;
        case 58: // Underline not supported, but eat colors
            sgr_color(term, idx, NULL);
            return;
        case 90: // bright foreground color
        case 91:
        case 92:
        case 93:
        case 94:
        case 95:
        case 96:
        case 97:
            term->fg_color = color256[param - 90 + 8];
            break;
        case 100: // bright foreground color
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            term->bg_color = color256[param - 100 + 8];
            break;
        }
    }
}

static void term_out_ht(term_state_t *term)
{
    if (term->x < term->width)
    {
        int xp = 8 - ((term->x + 8) & 7);
        term->ptr += xp;
        term->x += xp;
    }
}

static void term_out_lf(term_state_t *term)
{
    term->ptr += term->width;
    if (term->ptr >= term->mem + term->width * term->height)
        term->ptr -= term->width * term->height;
    if (++term->y == term->height)
    {
        --term->y;
        struct cell_state *line_ptr = term->ptr - term->x;
        for (size_t x = 0; x < term->width; x++)
        {
            line_ptr[x].glyph_code = ' ';
            line_ptr[x].fg_color = term->fg_color;
            line_ptr[x].bg_color = term->bg_color;
        }
        if (++term->y_offset == term->height)
            term->y_offset = 0;
    }
}

static void term_out_ff(term_state_t *term)
{
    term_state_clear(term);
}

static void term_out_cr(term_state_t *term)
{
    term->ptr -= term->x;
    term->x = 0;
}

static void term_out_char(term_state_t *term, char ch)
{
    if (term->x == term->width)
    {
        if (TERM_LINE_WRAP)
        {
            term_out_cr(term);
            term_out_lf(term);
        }
        else
        {
            --term->ptr;
            --term->x;
        }
    }
    term->x++;
    term->ptr->glyph_code = ch;
    term->ptr->fg_color = term->fg_color;
    term->ptr->bg_color = term->bg_color;
    term->ptr++;
}

// Cursor forward
static void term_out_cuf(term_state_t *term)
{
    if (term->csi_param_count > 1)
        return;
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->width - term->x)
        cols = term->width - term->x;
    term->ptr += cols;
    term->x += cols;
}

// Cursor backward
static void term_out_cub(term_state_t *term)
{
    if (term->csi_param_count > 1)
        return;
    uint16_t cols = term->csi_param[0];
    if (cols < 1)
        cols = 1;
    if (cols > term->x)
        cols = term->x;
    term->ptr -= cols;
    term->x -= cols;
}

// Delete characters
static void term_out_dch(term_state_t *term)
{
    if (term->csi_param_count > 1)
        return;
    uint16_t chars = term->csi_param[0];
    if (chars < 1)
        chars = 1;
    if (chars > term->width - term->x)
        chars = term->width - term->x;
    struct cell_state *tp = term->ptr;
    for (int i = term->x; i < term->width; i++)
    {
        if (chars + i >= term->width)
        {
            tp->glyph_code = ' ';
            tp->fg_color = term->fg_color;
            tp->bg_color = term->bg_color;
        }
        else
        {
            tp[0] = tp[chars];
        }
        ++tp;
    }
}

static void term_out_state_C0(term_state_t *term, char ch)
{
    if (ch == '\b')
    {
        if (term->x > 0)
            --term->ptr, --term->x;
    }
    else if (ch == '\t')
        term_out_ht(term);
    else if (ch == '\n')
        term_out_lf(term);
    else if (ch == '\f')
        term_out_ff(term);
    else if (ch == '\r')
        term_out_cr(term);
    else if (ch == '\33')
        term->ansi_state = ansi_state_Fe;
    else if (ch >= 32 && ch <= 255)
        term_out_char(term, ch);
}

static void term_out_state_Fe(term_state_t *term, char ch)
{
    if (ch == '[')
    {
        term->ansi_state = ansi_state_CSI;
        term->csi_param_count = 0;
        term->csi_param[0] = 0;
    }
    else
        term->ansi_state = ansi_state_C0;
}

static void term_out_state_CSI(term_state_t *term, char ch)
{
    // Silently discard overflow parameters but still count them.
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
        return;
    }
    term->ansi_state = ansi_state_C0;
    if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        term->csi_separator[term->csi_param_count] = 0;
    term->csi_param_count++;
    switch (ch)
    {
    case 'm':
        term_out_sgr(term);
        break;
    case 'C':
        term_out_cuf(term);
        break;
    case 'D':
        term_out_cub(term);
        break;
    case 'P':
        term_out_dch(term);
        break;
    }
}

static void term_out_chars(const char *buf, int length)
{
    if (length)
    {
        term_state_t *term = &term_console;
        term_cursor_set_inv(term, false);
        for (int i = 0; i < length; i++)
        {
            char ch = buf[i];
            if (ch == ANSI_CANCEL)
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
                case ansi_state_CSI:
                    term_out_state_CSI(term, ch);
                    break;
                }
        }
        term->timer = get_absolute_time();
    }
}

void term_init(void)
{
    // prepare console
    term_state_init(&term_console, 80, 32);
    // become part of stdout
    static stdio_driver_t term_stdio = {
        .out_chars = term_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
        .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
    };
    stdio_set_driver_enabled(&term_stdio, true);
}

void term_task(void)
{
    term_state_t *term = &term_console;
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, term->timer) < 0)
    {
        term_cursor_set_inv(term, !term->blink_state);
        // 0.3ms drift to avoid blinking cursor trearing
        term->timer = delayed_by_us(now, 499700);
    }
}

__attribute__((optimize("O1"))) void
term_render(struct scanvideo_scanline_buffer *dest, uint16_t height)
{
    term_state_t *term = &term_console;
    int line = scanvideo_scanline_number(dest->scanline_id);
    while (height <= term->y * 16)
    {
        line += 16;
        height += 16;
    }
    const uint8_t *font_line = &font16[(line & 15) * 256];
    line = line / 16 + term->y_offset;
    if (line >= term->height)
        line -= term->height;
    struct cell_state *term_ptr = term->mem + term->width * line - 1;
    uint32_t *buf = dest->data;
    for (int i = 0; i < term->width; i++)
    {
        ++term_ptr;
        uint8_t bits = font_line[term_ptr->glyph_code];
        uint16_t fg = term_ptr->fg_color;
        uint16_t bg = term_ptr->bg_color;

        switch (bits >> 4)
        {
        case 0:
            *++buf = bg | (bg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 1:
            *++buf = bg | (bg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 2:
            *++buf = bg | (bg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 3:
            *++buf = bg | (bg << 16);
            *++buf = fg | (fg << 16);
            break;
        case 4:
            *++buf = bg | (fg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 5:
            *++buf = bg | (fg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 6:
            *++buf = bg | (fg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 7:
            *++buf = bg | (fg << 16);
            *++buf = fg | (fg << 16);
            break;
        case 8:
            *++buf = fg | (bg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 9:
            *++buf = fg | (bg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 10:
            *++buf = fg | (bg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 11:
            *++buf = fg | (bg << 16);
            *++buf = fg | (fg << 16);
            break;
        case 12:
            *++buf = fg | (fg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 13:
            *++buf = fg | (fg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 14:
            *++buf = fg | (fg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 15:
            *++buf = fg | (fg << 16);
            *++buf = fg | (fg << 16);
            break;
        }

        switch (bits & 0xF)
        {
        case 0:
            *++buf = bg | (bg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 1:
            *++buf = bg | (bg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 2:
            *++buf = bg | (bg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 3:
            *++buf = bg | (bg << 16);
            *++buf = fg | (fg << 16);
            break;
        case 4:
            *++buf = bg | (fg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 5:
            *++buf = bg | (fg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 6:
            *++buf = bg | (fg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 7:
            *++buf = bg | (fg << 16);
            *++buf = fg | (fg << 16);
            break;
        case 8:
            *++buf = fg | (bg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 9:
            *++buf = fg | (bg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 10:
            *++buf = fg | (bg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 11:
            *++buf = fg | (bg << 16);
            *++buf = fg | (fg << 16);
            break;
        case 12:
            *++buf = fg | (fg << 16);
            *++buf = bg | (bg << 16);
            break;
        case 13:
            *++buf = fg | (fg << 16);
            *++buf = bg | (fg << 16);
            break;
        case 14:
            *++buf = fg | (fg << 16);
            *++buf = fg | (bg << 16);
            break;
        case 15:
            *++buf = fg | (fg << 16);
            *++buf = fg | (fg << 16);
            break;
        }
    }

    buf = (void *)dest->data;
    buf[0] = COMPOSABLE_RAW_RUN | (buf[1] << 16);
    buf[1] = 637 | (buf[1] & 0xFFFF0000);
    buf[321] = COMPOSABLE_RAW_1P | (0 << 16);
    buf[322] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data_used = 323;

    dest->data2[0] = COMPOSABLE_RAW_1P | (0 << 16);
    dest->data2[1] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data2_used = 2;

    dest->data3[0] = COMPOSABLE_RAW_1P | (0 << 16);
    dest->data3[1] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data3_used = 2;

    dest->status = SCANLINE_OK;
}
