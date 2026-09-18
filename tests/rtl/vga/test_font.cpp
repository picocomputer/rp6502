/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

extern "C"
{
#include "core/sys/sys.h"
}

#include "utest.h"

#include "core/term/color.h"
#include "core/term/font.h"

#include "aud_sine_tables.h"
#include "vid_font_tables.h"
#include "vid_palette_tables.h"

#include "core/aud/sine.h"


UTEST(font, generated_rom_matches_font_init)
{
    for (int i = 0; i < 4096; i++)
        ASSERT_EQ(VID_FONT16[i], font16[i]);
    for (int i = 0; i < 512; i++)
        ASSERT_EQ(VID_FONT_DEC16[i], font_dec_16[i]);
    for (size_t i = 0; i < sizeof(font_dec_8); i++)
        ASSERT_EQ(VID_FONT_DEC8[i], font_dec_8[i]);
    for (int i = 0; i < 2048; i++)
        ASSERT_EQ(VID_ITALIC16[i], italic16[i]);
    for (int i = 0; i < 2048; i++)
        ASSERT_EQ(VID_FONT8[i], font8[i]);
}

UTEST(font, generated_palettes_match_color_c)
{
    for (int i = 0; i < 2; i++)
        ASSERT_EQ(VID_COLOR_2[i], color_2[i]);
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(VID_COLOR_256[i], color_256[i]);
}

UTEST(font, generated_sine_matches_sine_init)
{
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(AUD_SINE_TABLE[i], sine_table[i]);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    sys_init();
    return utest_main(argc, argv);
}
