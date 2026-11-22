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

#include "font_tables.h"
#include "color_tables.h"


UTEST(font, generated_rom_matches_font_init)
{
    for (size_t i = 0; i < sizeof(font16); i++)
        ASSERT_EQ(FONT16[i], font16[i]);
    for (size_t i = 0; i < sizeof(font8); i++)
        ASSERT_EQ(FONT8[i], font8[i]);
}

UTEST(font, generated_palettes_match_color_c)
{
    for (int i = 0; i < 2; i++)
        ASSERT_EQ(COLOR_2[i], color_2[i]);
    for (int i = 0; i < 256; i++)
        ASSERT_EQ(COLOR_256[i], color_256[i]);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    sys_init();
    return utest_main(argc, argv);
}
