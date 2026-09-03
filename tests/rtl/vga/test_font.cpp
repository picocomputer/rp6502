/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The generated font ROM against the tables emu_core builds at runtime:
 * font_init lays out font16/font_dec_16/italic16 from the same source
 * arrays the generator parses, so every byte must agree after the
 * row-major to glyph-major re-index. Catches generator drift forever.
 *
 * No machine runs here. Both sides of every comparison are built from the
 * repository on this build: the generated headers by the same python the
 * fabric's packages come from, the runtime tables by emu_core's own
 * initializers. sys_init is called for exactly that — font_init fills
 * storage that is declared uninitialized, so without it these read whatever
 * was in it.
 */

extern "C"
{
/* emu_boot.h is the emulator suites' C header and carries no linkage guard of
 * its own; every other consumer is C. */
#include "core/sys/sys.h"
}

#include "utest.h"

#include "core/term/color.h"
#include "core/term/font.h"

#include "aud_sine_tables.h"
#include "vid_font_tables.h"
#include "vid_palette_tables.h"

#include "core/aud/sine.h"


/* The store is addressed the way font.c lays its tables out, so the
 * bitstream image and the firmware's are the same bytes in the same
 * order — which is what lets font_set_code_page's copies write the
 * store directly, and makes this a byte-for-byte comparison. */
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
    sys_init(); /* font_init and the rest of the table builders */
    return utest_main(argc, argv);
}
