/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The glyphs, moved rather than held: the renderer is the fabric and the
 * store is its memory, so the soft CPU only ever copies. That is why the
 * asset can carry seventeen code pages without spending code memory.
 *
 * The asset is row-major, so a page's high half is sixteen 128-byte runs
 * landing at row * 256 + 128. Every store is a whole word, because byte
 * lanes stop the fabric inferring a block RAM for the store.
 */

#include "font.h"
#include "mmio.h"

#include "vid_font_asset.h"

/* The staging bus serves a byte at a time. */
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

/* Rows are strided in the store and packed in the asset. */
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

static int font_find_page(uint16_t cp)
{
    for (int i = 0; i < VID_FONT_PAGE_COUNT; i++)
        if (VID_FONT_PAGES[i] == cp)
            return i;
    return -1;
}

bool font_has_code_page(uint16_t cp)
{
    return font_find_page(cp) >= 0;
}

/* A page with no glyphs blanks the high half rather than leaving the one
 * before it standing. */
void font_set_code_page(uint16_t cp)
{
    int page = font_find_page(cp);
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

/* The font store is written and never read, so a savestate cannot carry
 * it. It does not have to: every glyph in it came from the base faces
 * and one code page, and the page is an ordinary number in memory the
 * blob does carry. */
void font_restore(void)
{
    uint16_t cp = font_code_page;
    font_init();
    font_set_code_page(cp);
}

void font_init(void)
{
    font_copy(VID_FONT16, VID_FONT_OFF_FONT16, 4096);
    font_copy(VID_FONT8, VID_FONT_OFF_FONT8, 2048);
    font_copy(VID_ITALIC16, VID_FONT_OFF_ITALIC16, 2048);
    font_copy(VID_FONT_DEC16, VID_FONT_OFF_DEC16, 512);
    font_copy(VID_FONT_DEC8, VID_FONT_OFF_DEC8, 256);
    font_code_page = 0;
    font_set_code_page(437);
}
