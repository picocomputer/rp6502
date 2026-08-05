/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The glyphs, moved rather than held. On the VGA chip font.c keeps the
 * live tables in RAM because a software renderer reads them; here the
 * renderer is the fabric and the store is its memory, so the soft CPU
 * only ever copies. That is the whole reason the font asset can carry
 * seventeen code pages: none of them occupy code memory, and a page
 * change is three kilobytes moved from the staging window to the store.
 *
 * The asset is laid out in font.c's own row-major order, so a page's
 * high half arrives as sixteen 128-byte runs that land at row * 256 +
 * 128 — the destinations font_set_code_page's memcpys already use.
 * Every store is a whole word, because byte lanes are what stop the
 * fabric inferring a block RAM for the store at all.
 */

#include "font.h"
#include "mmio.h"

#include "vid_font_asset.h"

/* The staging bus serves a byte at a time, so a word is gathered rather
 * than read. */
static uint32_t font_word(uint32_t at)
{
    return (uint32_t)FONTS[at] | ((uint32_t)FONTS[at + 1] << 8)
           | ((uint32_t)FONTS[at + 2] << 16) | ((uint32_t)FONTS[at + 3] << 24);
}

static void font_copy(volatile uint32_t *dst, uint32_t at, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4)
        *dst++ = font_word(at + i);
}

static void font_blank(volatile uint32_t *dst, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 4)
        *dst++ = 0;
}

static uint16_t font_code_page;

/* A page's high half, or blanks when no page owns it: the rows are
 * strided in the store and packed in the asset. */
static void font_load_page(int page)
{
    for (int row = 0; row < 16; row++)
    {
        uint32_t to = ((uint32_t)row * 256 + 128) / 4;
        uint32_t at = VID_FONT_OFF_PAGES
                      + VID_FONT_PAGE_STRIDE * (uint32_t)page
                      + (uint32_t)row * 128;
        if (page < 0)
            font_blank(VID_FONT16 + to, 128);
        else
            font_copy(VID_FONT16 + to, at, 128);
        if (row >= 8)
            continue;
        if (page < 0)
            font_blank(VID_FONT8 + to, 128);
        else
            font_copy(VID_FONT8 + to, at + VID_FONT_PAGE_16, 128);
    }
}

void font_set_code_page(uint16_t cp)
{
    int page = -1;
    for (int i = 0; i < VID_FONT_PAGE_COUNT; i++)
        if (VID_FONT_PAGES[i] == cp)
        {
            page = i;
            break;
        }
    if (page < 0)
        cp = 0;
    if (font_code_page == cp)
        return;
    font_code_page = cp;
    font_load_page(page);
}

uint16_t font_get_code_page(void)
{
    return font_code_page;
}

void font_init(void)
{
    font_copy(VID_FONT16, VID_FONT_OFF_FONT16, 4096);
    font_copy(VID_FONT8, VID_FONT_OFF_FONT8, 2048);
    font_copy(VID_ITALIC16, VID_FONT_OFF_ITALIC16, 2048);
    font_copy(VID_FONT_DEC16, VID_FONT_OFF_DEC16, 512);
    font_code_page = 0;
    font_set_code_page(437);
}
