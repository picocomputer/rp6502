/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The generated font ROM against the tables emu_core builds at runtime:
 * font_init lays out font16/font_dec_16/italic16 from the same source
 * arrays the generator parses, so every byte must agree after the
 * row-major to glyph-major re-index. Catches generator drift forever.
 */

#include "oracle.h"
#include "utest.h"

#include "vga/term/color.h"
#include "vga/term/font.h"

#include "vid_font_tables.h"
#include "vid_palette_tables.h"

UTEST(font, generated_rom_matches_font_init)
{
    for (int code = 0; code < 256; code++)
        for (int row = 0; row < 16; row++)
            ASSERT_EQ(VID_FONT16[code * 16 + row], font16[row * 256 + code]);
    for (int idx = 0; idx < 0x20; idx++)
        for (int row = 0; row < 16; row++)
            ASSERT_EQ(VID_FONT_DEC16[idx * 16 + row],
                      font_dec_16[row * 32 + idx]);
    for (int code = 0; code < 128; code++)
        for (int row = 0; row < 16; row++)
            ASSERT_EQ(VID_ITALIC16[code * 16 + row],
                      italic16[row * 128 + code]);
}

UTEST(font, generated_palettes_match_color_c)
{
    for (int i = 0; i < 2; i++)
        ASSERT_EQ(VID_COLOR_2[i], color_2[i]);
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(VID_COLOR_256[i], color_256[i]);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    oracle_init();
    return utest_main(argc, argv);
}
