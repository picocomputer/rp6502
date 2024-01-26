/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "modes/modes.h"
#include "term/color.h"
#include "term/font.h"
#include "term/term.h"
#include "sys/vga.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include <stdio.h>

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
    ansi_state_CSI
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
    bool line_wrap;
    bool wrapped[TERM_MAX_HEIGHT];
    bool dirty[TERM_MAX_HEIGHT];
    bool cleaned;
    uint16_t erase_fg_color;
    uint16_t erase_bg_color;
    uint8_t y_offset;
    uint16_t fg_color;
    uint16_t bg_color;
    term_data_t *mem;
    term_data_t *ptr;
    absolute_time_t timer;
    int32_t blink_state;
    ansi_state_t ansi_state;
    uint16_t csi_param[TERM_CSI_PARAM_MAX_LEN];
    char csi_separator[TERM_CSI_PARAM_MAX_LEN];
    uint8_t csi_param_count;
} term_state_t;

static term_state_t term_40;
static term_state_t term_80;
static int16_t term_scanline_begin;

static void term_clean_line(term_state_t *term, uint8_t y)
{
    if (!term->dirty[y])
        return;
    term->dirty[y] = false;
    term_data_t *row = &term->mem[(term->y_offset + y) * term->width];
    if (row >= term->mem + term->width * TERM_MAX_HEIGHT)
        row -= term->width * TERM_MAX_HEIGHT;
    for (size_t i = 0; i < term->width; i++)
    {
        row[i].font_code = ' ';
        row[i].fg_color = term->erase_fg_color;
        row[i].bg_color = term->erase_bg_color;
    }
}

static void term_clean_task(term_state_t *term)
{
    // Clean only one line per task
    if (term->cleaned)
        return;
    for (size_t i = 0; i < term->height; i++)
        if (term->dirty[i])
        {
            term_clean_line(term, i);
            return;
        }
    term->cleaned = true;
}

static void term_state_clear(term_state_t *term)
{
    for (size_t i = 0; i < term->height; i++)
    {
        term->wrapped[i] = false;
        term->dirty[i] = true;
    }
    term->erase_fg_color = term->fg_color;
    term->erase_bg_color = term->bg_color;
    term->x = 0;
    term->y = 0;
    term->y_offset = 0;
    term->ptr = term->mem;
    term->cleaned = false;
    term_clean_line(term, 0);
}

static void term_state_init(term_state_t *term, uint8_t width, term_data_t *mem)
{
    term->width = width;
    term->height = TERM_STD_HEIGHT;
    term->line_wrap = true;
    term->mem = mem;
    term->fg_color = color_256[TERM_FG_COLOR_INDEX];
    term->bg_color = color_256[TERM_BG_COLOR_INDEX];
    term->blink_state = 0;
    term->ansi_state = ansi_state_C0;
    term_state_clear(term);
}

static void term_state_set_height(term_state_t *term, uint8_t height)
{
    assert(height >= 1 && height <= TERM_MAX_HEIGHT);
    while (height != term->height)
    {
        int row;
        if (height > term->height)
        {
            term->height++;
            if (term->y == term->height - 2)
            {
                term->y++;
                if (!term->y_offset)
                    term->y_offset = TERM_MAX_HEIGHT - 1;
                else
                    term->y_offset--;
                continue;
            }
            row = term->y_offset + term->height - 1;
        }
        else
        {
            term->height--;
            if (term->y == term->height)
            {
                term->y--;
                if (++term->y_offset >= TERM_MAX_HEIGHT)
                    term->y_offset -= TERM_MAX_HEIGHT;
                for (size_t i = 0; i < term->height; i++)
                    term->wrapped[i] = term->wrapped[i + 1];
                continue;
            }
            row = term->y_offset + term->height;
        }
        if (row >= TERM_MAX_HEIGHT)
            row -= TERM_MAX_HEIGHT;
        term_data_t *data = term->mem + row * term->width;
        for (size_t i = 0; i < term->width; i++)
        {
            data[i].font_code = ' ';
            data[i].fg_color = term->fg_color;
            data[i].bg_color = term->bg_color;
        }
    }
}

static void term_cursor_set_inv(term_state_t *term, bool inv)
{
    if (term->blink_state == -1 || inv == term->blink_state)
        return;
    term_data_t *term_ptr = term->ptr;
    if (term->x == term->width)
        term_ptr--;
    uint16_t swap = term_ptr->fg_color;
    term_ptr->fg_color = term_ptr->bg_color;
    term_ptr->bg_color = swap;
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
                *color = color_256[color_idx];
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
            term->fg_color = color_256[TERM_FG_COLOR_INDEX];
            term->bg_color = color_256[TERM_BG_COLOR_INDEX];
            break;
        case 1: // bold intensity
            for (int i = 0; i < 8; i++)
                if (term->fg_color == color_256[i])
                    term->fg_color = color_256[i + 8];
            break;
        case 22: // normal intensity
            for (int i = 8; i < 16; i++)
                if (term->fg_color == color_256[i])
                    term->fg_color = color_256[i - 8];
            break;
        case 30: // foreground color
        case 31:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 37:
            term->fg_color = color_256[param - 30];
            break;
        case 38:
            sgr_color(term, idx, &term->fg_color);
            return;
        case 39:
            term->fg_color = color_256[TERM_FG_COLOR_INDEX];
            break;
        case 40: // background color
        case 41:
        case 42:
        case 43:
        case 44:
        case 45:
        case 46:
        case 47:
            term->bg_color = color_256[param - 40];
            break;
        case 48:
            sgr_color(term, idx, &term->bg_color);
            return;
        case 49:
            term->bg_color = color_256[TERM_BG_COLOR_INDEX];
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
            term->fg_color = color_256[param - 90 + 8];
            break;
        case 100: // bright background color
        case 101:
        case 102:
        case 103:
        case 104:
        case 105:
        case 106:
        case 107:
            term->bg_color = color_256[param - 100 + 8];
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

static void term_out_lf(term_state_t *term, bool wrapping)
{
    term->ptr += term->width;
    if (term->ptr >= term->mem + term->width * TERM_MAX_HEIGHT)
        term->ptr -= term->width * TERM_MAX_HEIGHT;
    if (wrapping)
        term->wrapped[term->y] = wrapping;
    else if (term->wrapped[term->y])
    {
        ++term->y;
        return term_out_lf(term, false);
    }
    if (++term->y == term->height)
    {
        --term->y;
        term_data_t *line_ptr = term->ptr - term->x;
        for (size_t x = 0; x < term->width; x++)
        {
            line_ptr[x].font_code = ' ';
            line_ptr[x].fg_color = term->fg_color;
            line_ptr[x].bg_color = term->bg_color;
        }
        if (++term->y_offset == TERM_MAX_HEIGHT)
            term->y_offset = 0;
        // scroll the wrapped and dirty flags
        for (size_t y = 0; y < term->height - 1; y++)
        {
            term->wrapped[y] = term->wrapped[y + 1];
            term->dirty[y] = term->dirty[y + 1];
        }
        term->wrapped[term->height - 1] = false;
        term->dirty[term->height - 1] = false;
    }
    term_clean_line(term, term->y);
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

static void term_out_glyph(term_state_t *term, char ch)
{
    if (term->x == term->width)
    {
        if (term->line_wrap)
        {
            term_out_cr(term);
            term_out_lf(term, true);
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

// Cursor up by one line
static void term_out_cuu_1(term_state_t *term)
{
    if (term->y)
    {
        term->y--;
        term->ptr -= term->width;
        if (term->ptr < 0)
            term->ptr += term->width * TERM_MAX_HEIGHT;
    }
}

// Cursor forward
static void term_out_cuf(term_state_t *term)
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
            term_out_cr(term);
            term_out_lf(term, true);
            return term_out_cuf(term);
        }
        else
            cols = term->width - term->x;
    }
    term->ptr += cols;
    term->x += cols;
}

// Cursor backward
static void term_out_cub(term_state_t *term)
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
            term->ptr += term->width - term->x;
            term->x += term->width - term->x;
            term_out_cuu_1(term);
            return term_out_cub(term);
        }
        else
            cols = term->x;
    }
    term->ptr -= cols;
    term->x -= cols;
}

// Delete characters
static void term_out_dch(term_state_t *term)
{
    unsigned max_chars = term->width - term->x;
    for (unsigned i = term->y; i < term->height - 1; i++)
        if (term->wrapped[i])
            max_chars += term->width;
    uint16_t chars = term->csi_param[0];
    if (chars < 1)
        chars = 1;
    if (chars > max_chars)
        chars = max_chars;

    term_data_t *tp_max = term->mem + term->width * TERM_MAX_HEIGHT;
    term_data_t *tp_dst = term->ptr;
    term_data_t *tp_src = &term->ptr[chars];
    if (tp_src >= tp_max)
        tp_src -= term->width * TERM_MAX_HEIGHT;
    for (unsigned i = 0; i < max_chars - chars; i++)
    {
        tp_dst[0] = tp_src[0];
        if (++tp_dst >= tp_max)
            tp_dst -= term->width * TERM_MAX_HEIGHT;
        if (++tp_src >= tp_max)
            tp_src -= term->width * TERM_MAX_HEIGHT;
    }
    for (unsigned i = max_chars - chars; i < max_chars; i++)
    {
        tp_dst->font_code = ' ';
        tp_dst->fg_color = term->fg_color;
        tp_dst->bg_color = term->bg_color;
        if (++tp_dst >= tp_max)
            tp_dst -= term->width * TERM_MAX_HEIGHT;
    }
}

static void term_out_state_C0(term_state_t *term, char ch)
{
    if (ch == '\b')
    {
        term->csi_param[0] = 1;
        term_out_cub(term);
    }
    else if (ch == '\t')
        term_out_ht(term);
    else if (ch == '\n')
        term_out_lf(term, false);
    else if (ch == '\f')
        term_out_ff(term);
    else if (ch == '\r')
        term_out_cr(term);
    else if (ch == '\33')
        term->ansi_state = ansi_state_Fe;
    else if (ch >= 32 && ch <= 255)
        term_out_glyph(term, ch);
}

static void term_out_state_Fe(term_state_t *term, char ch)
{
    if (ch == '[')
    {
        term->ansi_state = ansi_state_CSI;
        term->csi_param_count = 0;
        term->csi_param[0] = 0;
    }
    else if (ch == 'N')
        term->ansi_state = ansi_state_SS2;
    else if (ch == 'O')
        term->ansi_state = ansi_state_SS3;
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

static void term_out_state_CSI(term_state_t *term, char ch)
{
    // Silently discard overflow parameters but still count to + 1.
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
    term->ansi_state = ansi_state_C0;
    if (term->csi_param_count < TERM_CSI_PARAM_MAX_LEN)
        term->csi_separator[term->csi_param_count] = 0;
    if (++term->csi_param_count > TERM_CSI_PARAM_MAX_LEN)
        term->csi_param_count = TERM_CSI_PARAM_MAX_LEN;
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
            term_out_state_CSI(term, ch);
            break;
        }
}

static void term_out_chars(const char *buf, int length)
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
        term_40.timer = term_80.timer = get_absolute_time();
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
        .out_chars = term_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
        .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
    };
    stdio_set_driver_enabled(&term_stdio, true);
}

static void term_blink_cursor(term_state_t *term)
{
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, term->timer) < 0)
    {
        term_cursor_set_inv(term, !term->blink_state);
        // 0.3ms drift to avoid blinking cursor trearing
        if (term->x == term->width)
            // fast blink when off right side
            term->timer = delayed_by_us(now, 249700);
        else
            term->timer = delayed_by_us(now, 499700);
    }
}

void term_task(void)
{
    term_blink_cursor(&term_40);
    term_blink_cursor(&term_80);
    term_clean_task(&term_40);
    term_clean_task(&term_80);
}

static inline bool __attribute__((optimize("O1")))
term_render_320(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= term_scanline_begin;
    const uint8_t *font_line = &font8[(scanline_id & 7) * 256];
    int mem_y = scanline_id / 8 + term_40.y_offset;
    if (mem_y >= TERM_MAX_HEIGHT)
        mem_y -= TERM_MAX_HEIGHT;
    term_data_t *term_ptr = term_40.mem + 40 * mem_y;
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

static inline bool __attribute__((optimize("O1")))
term_render_640(int16_t scanline_id, uint16_t *rgb)
{
    scanline_id -= term_scanline_begin;
    const uint8_t *font_line = &font16[(scanline_id & 15) * 256];
    int mem_y = scanline_id / 16 + term_80.y_offset;
    if (mem_y >= TERM_MAX_HEIGHT)
        mem_y -= TERM_MAX_HEIGHT;
    term_data_t *term_ptr = term_80.mem + 80 * mem_y;
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

static bool __attribute__((optimize("O1")))
term_render(int16_t scanline_id, int16_t width, uint16_t *rgb, uint16_t config_ptr)
{
    if (width == 320)
        return term_render_320(scanline_id, rgb);
    else
        return term_render_640(scanline_id, rgb);
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
