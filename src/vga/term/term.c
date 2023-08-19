/*
 * Copyright (c) 2023 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "ansi.h"
#include "font.h"
#include "term.h"
#include "pico/stdlib.h"
#include "pico/stdio/driver.h"
#include "pico/scanvideo.h"
#include "pico/scanvideo/composable_scanline.h"
#include <stdio.h>

// If you are extending this for use outside the Picocomputer,
// CSI codes with multiple parameters will need a more complete
// implementation first.

#define TERM_WIDTH 80
#define TERM_HEIGHT 32
#define TERM_MEM_SIZE (TERM_WIDTH * TERM_HEIGHT * 2)
#define TERM_WORD_WRAP 1
static int term_x = 0, term_y = 0;
static int term_y_offset = 0;
static uint8_t term_color = 0x07;
static uint32_t term_color_data[1024];
static uint8_t term_memory[TERM_MEM_SIZE];
static uint8_t *term_ptr = term_memory;
static absolute_time_t term_timer = {0};
static int32_t term_blink_state = 0;
static ansi_state_t term_state = ansi_state_C0;
static int term_csi_param;

static void term_cursor_set_inv(bool inv)
{
    if (term_blink_state == -1 || inv == term_blink_state || term_x >= TERM_WIDTH)
        return;
    term_ptr[1] = ((term_ptr[1] & 0x0F) << 4) | ((term_ptr[1] & 0xF0) >> 4);
    term_blink_state = inv;
}

static void term_out_sgr(int param)
{
    switch (param)
    {
    case -1:
    case 0: // reset
        term_color = 0x07;
        break;
    case 1: // bold intensity
        term_color = (term_color | 0x08);
        break;
    case 22: // normal intensity
        term_color = (term_color & 0xf7);
        break;
    case 30: // foreground color
    case 31:
    case 32:
    case 33:
    case 34:
    case 35:
    case 36:
    case 37:
        term_color = (term_color & 0xf8) | (param - 30);
        break;
    case 40: // background color
    case 41:
    case 42:
    case 43:
    case 44:
    case 45:
    case 46:
    case 47:
        term_color = ((param - 40) << 4) | (term_color & 0x8f);
        break;
    }
}

static void term_out_ht()
{
    if (term_x < TERM_WIDTH)
    {
        int xp = 8 - ((term_x + 8) & 7);
        term_ptr += xp * 2;
        term_x += xp;
    }
}

static void term_out_lf()
{
    term_ptr += TERM_WIDTH * 2;
    if (term_ptr >= term_memory + TERM_MEM_SIZE)
    {
        term_ptr -= TERM_MEM_SIZE;
    }

    if (++term_y == TERM_HEIGHT)
    {
        term_y = TERM_HEIGHT - 1;
        if (++term_y_offset == TERM_HEIGHT)
        {
            term_y_offset = 0;
        }
        uint8_t *line_ptr = term_ptr - term_x * 2;
        for (size_t x = 0; x < TERM_WIDTH * 2; x += 2)
        {
            line_ptr[x] = ' ';
            line_ptr[x + 1] = term_color;
        }
    }
}

static void term_out_ff()
{
    term_ptr -= term_x * 2;
    term_ptr += (TERM_HEIGHT - term_y) * TERM_WIDTH * 2;
    term_x = term_y = 0;
    if (term_ptr >= term_memory + TERM_MEM_SIZE)
    {
        term_ptr -= TERM_MEM_SIZE;
    }
    for (size_t i = 0; i < TERM_MEM_SIZE; i += 2)
    {
        term_memory[i] = ' ';
        term_memory[i + 1] = term_color;
    }
}

static void term_out_cr()
{
    term_ptr -= term_x * 2;
    term_x = 0;
}

static void term_out_char(char ch)
{
    if (term_x == TERM_WIDTH)
    {
        if (TERM_WORD_WRAP)
        {
            term_out_cr();
            term_out_lf();
        }
        else
        {
            term_ptr -= 2;
            term_x -= 1;
        }
    }
    term_x++;
    *term_ptr++ = ch;
    *term_ptr++ = term_color;
}

// Cursor forward
static void term_out_cuf(int cols)
{
    if (cols > TERM_WIDTH - term_x)
        cols = TERM_WIDTH - term_x;
    term_ptr += cols * 2;
    term_x += cols;
}

// Cursor backward
static void term_out_cub(int cols)
{
    if (cols > term_x)
        cols = term_x;
    term_ptr -= cols * 2;
    term_x -= cols;
}

// Delete characters
static void term_out_dch(int chars)
{
    uint8_t *tp = term_ptr;
    if (chars > TERM_WIDTH - term_x)
        chars = TERM_WIDTH - term_x;
    for (int i = term_x; i < TERM_WIDTH; i++)
    {
        if (chars + i >= TERM_WIDTH)
        {
            tp[0] = ' ';
            tp[1] = term_color;
        }
        else
        {
            tp[0] = tp[chars * 2];
            tp[1] = tp[chars * 2 + 1];
        }
        tp += 2;
    }
}

static void term_out_state_C0(char ch)
{
    if (ch == '\b')
        term_out_cub(1);
    else if (ch == '\t')
        term_out_ht();
    else if (ch == '\n')
        term_out_lf();
    else if (ch == '\f')
        term_out_ff();
    else if (ch == '\r')
        term_out_cr();
    else if (ch == '\33')
        term_state = ansi_state_Fe;
    else if (ch >= 32 && ch <= 255)
        term_out_char(ch);
}

static void term_out_state_Fe(char ch)
{
    if (ch == '[')
    {
        term_state = ansi_state_CSI;
        term_csi_param = -1;
    }
    else
        term_state = ansi_state_C0;
}

static void term_out_state_CSI(char ch)
{
    if (ch >= '0' && ch <= '9')
    {
        if (term_csi_param < 0)
        {
            term_csi_param = ch - '0';
        }
        else
        {
            term_csi_param *= 10;
            term_csi_param += ch - '0';
        }
        return;
    }
    if (ch == ';')
    {
        // all codes with multiple parameters
        // end up here where we assume SGR
        term_out_sgr(term_csi_param);
        term_csi_param = -1;
        return;
    }
    term_state = ansi_state_C0;
    if (ch == 'm')
    {
        term_out_sgr(term_csi_param);
        return;
    }
    // Everything below defaults to 1
    if (term_csi_param < 0)
        term_csi_param = -term_csi_param;
    if (ch == 'C')
        term_out_cuf(term_csi_param);
    else if (ch == 'D')
        term_out_cub(term_csi_param);
    else if (ch == 'P')
        term_out_dch(term_csi_param);
}

static void term_out_chars(const char *buf, int length)
{
    if (length)
    {
        term_cursor_set_inv(false);
        for (int i = 0; i < length; i++)
        {
            char ch = buf[i];
            if (ch == ANSI_CANCEL)
                term_state = ansi_state_C0;
            else
                switch (term_state)
                {
                case ansi_state_C0:
                    term_out_state_C0(ch);
                    break;
                case ansi_state_Fe:
                    term_out_state_Fe(ch);
                    break;
                case ansi_state_CSI:
                    term_out_state_CSI(ch);
                    break;
                }
        }
        term_timer = get_absolute_time();
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
    // become part of stdout
    stdio_set_driver_enabled(&term_stdio, true);
    // populate color lookup table
    uint32_t colors[] = {
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 205, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(205, 0, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 205, 205),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(229, 229, 229),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(127, 127, 127),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 0),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 0, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 0, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(0, 255, 255),
        PICO_SCANVIDEO_PIXEL_FROM_RGB8(255, 255, 255),
    };
    for (int c = 0; c < 256; c++)
    {
        uint32_t fgcolor = colors[c & 0x0f];
        uint32_t bgcolor = colors[(c & 0xf0) >> 4];
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
    absolute_time_t now = get_absolute_time();
    if (absolute_time_diff_us(now, term_timer) < 0)
    {
        term_cursor_set_inv(!term_blink_state);
        // 0.3ms drift to avoid blinking cursor trearing
        term_timer = delayed_by_us(now, 499700);
    }
}

void term_clear(void)
{
    // reset state and clear screen
    puts("\30\33[0m\f");
}

void term_render(struct scanvideo_scanline_buffer *dest, uint16_t height)
{
    // renders 80 columns into 640 pixels with 16 fg/bg colors
    // requires PICO_SCANVIDEO_MAX_SCANLINE_BUFFER_WORDS=323
    int line = scanvideo_scanline_number(dest->scanline_id);
    while (height <= term_y * 16)
    {
        line += 16;
        height += 16;
    }
    const uint8_t *font_line = &font16[(line & 15) * 256];
    line = line / 16 + term_y_offset;
    if (line >= TERM_HEIGHT)
        line -= TERM_HEIGHT;
    uint8_t *term_ptr = term_memory + TERM_WIDTH * 2 * line;
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
    buf[321] = COMPOSABLE_RAW_1P | 0;
    buf[322] = COMPOSABLE_EOL_SKIP_ALIGN;
    dest->data_used = 323;
    dest->status = SCANLINE_OK;
}
