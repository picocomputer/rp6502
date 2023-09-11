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

#define TERM_WIDTH 80
#define TERM_HEIGHT 32
#define TERM_MEM_SIZE (TERM_WIDTH * TERM_HEIGHT * 2)
#define TERM_WORD_WRAP 1

static uint32_t term_color_data[1024];

typedef struct term_state
{
    uint16_t x;
    uint16_t y;
    uint16_t y_offset;
    uint8_t color;
    uint8_t memory[TERM_MEM_SIZE];
    uint8_t *ptr;
    absolute_time_t timer;
    int32_t blink_state;
    ansi_state_t state;
    int csi_param; // TODO make list
} term_state_t;

static term_state_t term_console;

static void term_state_reset(term_state_t *term)
{
    term->x = 0;
    term->y = 0;
    term->y_offset = 0;
    term->color = 0x07;
    term->memory[TERM_MEM_SIZE];
    term->ptr = term->memory;
    term->blink_state = 0;
    term->state = ansi_state_C0;
}

static void term_cursor_set_inv(term_state_t *term, bool inv)
{
    if (term->blink_state == -1 || inv == term->blink_state || term->x >= TERM_WIDTH)
        return;
    term->ptr[1] = ((term->ptr[1] & 0x0F) << 4) | ((term->ptr[1] & 0xF0) >> 4);
    term->blink_state = inv;
}

static void term_out_sgr(term_state_t *term, int param)
{
    switch (param)
    {
    case -1:
    case 0: // reset
        term->color = 0x07;
        break;
    case 1: // bold intensity
        term->color = (term->color | 0x08);
        break;
    case 22: // normal intensity
        term->color = (term->color & 0xf7);
        break;
    case 30: // foreground color
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
        term->color = (term->color & 0xf8) | (param - 30);
        break;
    case 40: // background color
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
        term->color = ((param - 40) << 4) | (term->color & 0x8f);
        break;
    }
}

static void term_out_ht(term_state_t *term)
{
    if (term->x < TERM_WIDTH)
    {
        int xp = 8 - ((term->x + 8) & 7);
        term->ptr += xp * 2;
        term->x += xp;
    }
}

static void term_out_lf(term_state_t *term)
{
    term->ptr += TERM_WIDTH * 2;
    if (term->ptr >= term->memory + TERM_MEM_SIZE)
    {
        term->ptr -= TERM_MEM_SIZE;
    }

    if (++term->y == TERM_HEIGHT)
    {
        term->y = TERM_HEIGHT - 1;
        if (++term->y_offset == TERM_HEIGHT)
        {
            term->y_offset = 0;
        }
        uint8_t *line_ptr = term->ptr - term->x * 2;
        for (size_t x = 0; x < TERM_WIDTH * 2; x += 2)
        {
            line_ptr[x] = ' ';
            line_ptr[x + 1] = term->color;
        }
    }
}

static void term_out_ff(term_state_t *term)
{
    term->ptr -= term->x * 2;
    term->ptr += (TERM_HEIGHT - term->y) * TERM_WIDTH * 2;
    term->x = term->y = 0;
    if (term->ptr >= term->memory + TERM_MEM_SIZE)
    {
        term->ptr -= TERM_MEM_SIZE;
    }
    for (size_t i = 0; i < TERM_MEM_SIZE; i += 2)
    {
        term->memory[i] = ' ';
        term->memory[i + 1] = term->color;
    }
}

static void term_out_cr(term_state_t *term)
{
    term->ptr -= term->x * 2;
    term->x = 0;
}

static void term_out_char(term_state_t *term, char ch)
{
    if (term->x == TERM_WIDTH)
    {
        if (TERM_WORD_WRAP)
        {
            term_out_cr(term);
            term_out_lf(term);
        }
        else
        {
            term->ptr -= 2;
            term->x -= 1;
        }
    }
    term->x++;
    *term->ptr++ = ch;
    *term->ptr++ = term->color;
}

// Cursor forward
static void term_out_cuf(term_state_t *term, int cols)
{
    if (cols > TERM_WIDTH - term->x)
        cols = TERM_WIDTH - term->x;
    term->ptr += cols * 2;
    term->x += cols;
}

// Cursor backward
static void term_out_cub(term_state_t *term, int cols)
{
    if (cols > term->x)
        cols = term->x;
    term->ptr -= cols * 2;
    term->x -= cols;
}

// Delete characters
static void term_out_dch(term_state_t *term, int chars)
{
    uint8_t *tp = term->ptr;
    if (chars > TERM_WIDTH - term->x)
        chars = TERM_WIDTH - term->x;
    for (int i = term->x; i < TERM_WIDTH; i++)
    {
        if (chars + i >= TERM_WIDTH)
        {
            tp[0] = ' ';
            tp[1] = term->color;
        }
        else
        {
            tp[0] = tp[chars * 2];
            tp[1] = tp[chars * 2 + 1];
        }
        tp += 2;
    }
}

static void term_out_state_C0(term_state_t *term, char ch)
{
    if (ch == '\b')
        term_out_cub(term, 1);
    else if (ch == '\t')
        term_out_ht(term);
    else if (ch == '\n')
        term_out_lf(term);
    else if (ch == '\f')
        term_out_ff(term);
    else if (ch == '\r')
        term_out_cr(term);
    else if (ch == '\33')
        term->state = ansi_state_Fe;
    else if (ch >= 32 && ch <= 255)
        term_out_char(term, ch);
}

static void term_out_state_Fe(term_state_t *term, char ch)
{
    if (ch == '[')
    {
        term->state = ansi_state_CSI;
        term->csi_param = -1;
    }
    else
        term->state = ansi_state_C0;
}

static void term_out_state_CSI(term_state_t *term, char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        if (term->csi_param < 0)
        {
            term->csi_param = ch - '0';
        }
        else
        {
            term->csi_param *= 10;
            term->csi_param += ch - '0';
        }
        return;
    }
    if (ch == ';')
    {
        // all codes with multiple parameters
        // end up here where we assume SGR
        term_out_sgr(term, term->csi_param);
        term->csi_param = -1;
        return;
    }
    term->state = ansi_state_C0;
    if (ch == 'm')
    {
        term_out_sgr(term, term->csi_param);
        return;
    }
    // Everything below defaults to 1
    if (term->csi_param < 0)
        term->csi_param = -term->csi_param;
    if (ch == 'C')
        term_out_cuf(term, term->csi_param);
    else if (ch == 'D')
        term_out_cub(term, term->csi_param);
    else if (ch == 'P')
        term_out_dch(term, term->csi_param);
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
                term->state = ansi_state_C0;
            else
                switch (term->state)
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

static stdio_driver_t term_stdio = {
    .out_chars = term_out_chars,
#if PICO_STDIO_ENABLE_CRLF_SUPPORT
    .crlf_enabled = PICO_STDIO_DEFAULT_CRLF
#endif
};

void term_init(void)
{
    term_state_reset(&term_console);

    // become part of stdout
    stdio_set_driver_enabled(&term_stdio, true);
    for (int c = 0; c < 256; c++)
    {
        uint32_t fgcolor = color256[c & 0x0f];
        uint32_t bgcolor = color256[(c & 0xf0) >> 4];
        size_t pos = c * 4;
        term_color_data[pos] = bgcolor | (bgcolor << 16);
        term_color_data[pos + 1] = bgcolor | (fgcolor << 16);
        term_color_data[pos + 2] = fgcolor | (bgcolor << 16);
        term_color_data[pos + 3] = fgcolor | (fgcolor << 16);
    }
    term_clear();
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

void term_clear(void)
{
    // reset state and clear screen
    printf("\30\33[0m\f");
}

void term_render(struct scanvideo_scanline_buffer *dest, uint16_t height)
{
    // renders 80 columns into 640 pixels with 16 fg/bg colors
    // requires PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    term_state_t *term = &term_console;
    int line = scanvideo_scanline_number(dest->scanline_id);
    while (height <= term->y * 16)
    {
        line += 16;
        height += 16;
    }
    const uint8_t *font_line = &font16[(line & 15) * 256];
    line = line / 16 + term->y_offset;
    if (line >= TERM_HEIGHT)
        line -= TERM_HEIGHT;
    uint8_t *term_ptr = term->memory + TERM_WIDTH * 2 * line;
    uint32_t *buf = dest->data;
    for (int i = 0; i < TERM_WIDTH * 2; i += 2)
    {
        uint8_t bits = font_line[term_ptr[i]];
        uint32_t *colors = term_color_data + term_ptr[i + 1] * 4;
        *++buf = colors[bits >> 6];
        *++buf = colors[bits >> 4 & 0x03];
        *++buf = colors[bits >> 2 & 0x03];
        *++buf = colors[bits & 0x03];
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
