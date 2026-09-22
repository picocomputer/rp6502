/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "corpus.h"
#include "host/host.h"
#include "mut.h"
#include "utest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t settled[640 * 480];

static void run_case(int *utest_result, const char *name, uint32_t expect,
                     mut_budget_t claim)
{
    int w, h;
    ASSERT_TRUE(corpus_size(name, &w, &h));
    const size_t px = (size_t)w * (size_t)h;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.rp6502", ROMS_DIR, name);
    ASSERT_TRUE(mut_boot(path));

    memcpy(settled, mut_frame(w, h), px * sizeof(uint32_t));
    ASSERT_EQ(memcmp(settled, mut_frame(w, h), px * sizeof(uint32_t)), 0);

    uint32_t got = host_crc32(0, settled, px * sizeof(uint32_t));
    if (getenv("RP6502_BLESS_CRC"))
        printf("    0x%08X,  /* %s */\n", got, name);
    else if (got != expect)
    {
        fprintf(stderr, "%s: frame crc 0x%08X, expected 0x%08X\n",
                name, got, expect);
        ASSERT_EQ(got, expect);
    }

    mut_budget_t b = mut_measure(name);
    if (claim != MUT_BUDGET_NONE && b != MUT_BUDGET_NONE)
        ASSERT_EQ((int)b, (int)claim);
}

UTEST(mode1, bpp1_8x8_builtin_640x480)
{
    run_case(utest_result, "mode1_1bpp8x8", 0x8D4B4FCA, MUT_BUDGET_NONE);
}

UTEST(mode1, bpp4_8x16_xram_palette_320x240)
{
    run_case(utest_result, "mode1_4bpp8x16", 0x548FBBD3, MUT_BUDGET_NONE);
}

UTEST(mode1, bpp4r_8x8_builtin_320x180)
{
    run_case(utest_result, "mode1_4bppr8x8", 0x59790C33, MUT_BUDGET_NONE);
}

UTEST(mode1, bpp8_8x8_xram_font_320x240)
{
    run_case(utest_result, "mode1_8bpp8x8", 0xDA09EB2B, MUT_BUDGET_NONE);
}

UTEST(mode1, bpp16_8x16_640x360)
{
    run_case(utest_result, "mode1_16bpp8x16", 0x2461E272, MUT_BUDGET_UNDER);
}

UTEST(mode1, bpp1_wrap_halfword_palette_320x240)
{
    run_case(utest_result, "mode1_wrap", 0x67DD7684, MUT_BUDGET_NONE);
}

UTEST(mode2, bpp1_8px_builtin_640x480)
{
    run_case(utest_result, "mode2_1bpp8", 0x7BE4AB68, MUT_BUDGET_NONE);
}

UTEST(mode2, bpp2_16px_xram_palette_320x240)
{
    run_case(utest_result, "mode2_2bpp16", 0x30EF7C61, MUT_BUDGET_NONE);
}

UTEST(mode2, bpp4_8px_trimmed_320x240)
{
    run_case(utest_result, "mode2_4bpp8trim", 0xCACDE2AD, MUT_BUDGET_NONE);
}

UTEST(mode2, bpp8_16px_wrap_320x180)
{
    run_case(utest_result, "mode2_8bpp16wrap", 0x583C97D5, MUT_BUDGET_NONE);
}

UTEST(mode2, composite_three_planes_320x240)
{
    run_case(utest_result, "mode2_composite", 0x45EC0189, MUT_BUDGET_UNDER);
}

UTEST(mode2, bpp2_16px_both_trims_320x240)
{
    run_case(utest_result, "mode2_16trim", 0xB56A47F9, MUT_BUDGET_NONE);
}

UTEST(mode2, bpp4_8px_xtrim_640x480)
{
    run_case(utest_result, "mode2_trimx", 0x5C17A168, MUT_BUDGET_NONE);
}

UTEST(mode2, bpp8_8px_xtrim_320x180)
{
    run_case(utest_result, "mode2_trimx8", 0x2C2E39F0, MUT_BUDGET_NONE);
}

UTEST(mode2, bpp1_8px_ytrim_320x180)
{
    run_case(utest_result, "mode2_trimy", 0x70ABB716, MUT_BUDGET_NONE);
}

/* The tile pointer is not checked when the mode is programmed, because mode 2
 * does not require a full tile set in XRAM. The last tiles here are addressed
 * past the end, so this pins the tile both the renderer and mode2.sv leave
 * transparent black, and keeps the renderer off the far side of the array. */
UTEST(mode2, tile_off_the_end_320x240)
{
    run_case(utest_result, "mode2_tileoob", 0xD03C4762, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp8_xram_palette_640x480)
{
    run_case(utest_result, "mode3_8bpp", 0x6B55D171, MUT_BUDGET_UNDER);
}

UTEST(mode3, two_bpp8_fills_serial_640x480)
{
    run_case(utest_result, "fill_heavy640", 0x42E2D810, MUT_BUDGET_UNDER);
}

UTEST(mode3, three_bpp8_fills_serial_640x480)
{
    run_case(utest_result, "fill_three640", 0x6ABDA34F, MUT_BUDGET_UNDER);
}

UTEST(mode1, three_bpp8_8x8_text_planes_640x480)
{
    run_case(utest_result, "text_three640", 0x982FCF90, MUT_BUDGET_UNDER);
}

UTEST(mode3, three_bpp16_fills_serial_640x480)
{
    run_case(utest_result, "fill_three640_16bpp", 0xCECCF650, MUT_BUDGET_UNDER);
}

/* Fill and sprites have a clock budget each, but one XRAM port between them,
 * and three 16bpp fills ask that port for a word almost every clock. */
UTEST(mode4, sprites_over_three_bpp16_fills_640x480)
{
    run_case(utest_result, "fill_three640_16bpp_spr", 0xFE3BF35C,
             MUT_BUDGET_NONE);
}

/* The odd bitmap's words start eight bits into a pixel and the even one's do
 * not, so this pins that the fill tail carries no bit phase from one plane
 * into the next. */
UTEST(mode3, two_bpp16_planes_of_opposite_byte_parity_640x480)
{
    run_case(utest_result, "mode3_16parity", 0x4BEB0F74, MUT_BUDGET_UNDER);
}

UTEST(mode3, bpp16_odd_data_wrap_640x360)
{
    run_case(utest_result, "mode3_16odd_wrap", 0xFA27DA4F, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp1_builtin_320x240)
{
    run_case(utest_result, "mode3_1bpp", 0x4EA78B8D, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp4_reversed_320x180)
{
    run_case(utest_result, "mode3_4bppr", 0x5DA4EDF2, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp16_640x360)
{
    run_case(utest_result, "mode3_16bpp", 0x4C6E85C8, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp2_halfword_ptrs_320x240)
{
    run_case(utest_result, "mode3_2bpp", 0x4EC17E37, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp4_builtin_640x480)
{
    run_case(utest_result, "mode3_4bpp", 0x5448EB4A, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp1_reversed_320x180)
{
    run_case(utest_result, "mode3_1bppr", 0x026FBB1F, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp2_reversed_lower_640x360)
{
    run_case(utest_result, "mode3_2bppr", 0x50ADCE41, MUT_BUDGET_NONE);
}

UTEST(mode3, bpp8_wrap_bound_ptrs_320x240)
{
    run_case(utest_result, "mode3_wrap", 0x315F62CC, MUT_BUDGET_NONE);
}

/* mode3_16bpp_odd is mode3_16bpp with its bitmap at the odd address $0801,
 * so the two frames have the same CRC. */
UTEST(mode3, bpp16_odd_data_640x360)
{
    run_case(utest_result, "mode3_16bpp_odd", 0x4C6E85C8, MUT_BUDGET_NONE);
}

UTEST(mode4, log3_sprite_only_320x240)
{
    run_case(utest_result, "mode4_8", 0x884F54F6, MUT_BUDGET_NONE);
}

UTEST(mode4, log4_metadata_over_fill_320x240)
{
    run_case(utest_result, "mode4_meta16", 0x50A87FF3, MUT_BUDGET_NONE);
}

UTEST(mode4, log5_cross_plane_640x480)
{
    run_case(utest_result, "mode4_32", 0xC86F275E, MUT_BUDGET_UNDER);
}

UTEST(mode4, log6_plane2_320x180)
{
    run_case(utest_result, "mode4_64", 0x337C7EAA, MUT_BUDGET_NONE);
}

UTEST(mode4, affine_identity_320x240)
{
    run_case(utest_result, "mode4a_id", 0x37028F77, MUT_BUDGET_NONE);
}

/* The identity matrix maps the sprite onto its image 1:1, so an affine blit
 * has to put every texel where the plain blit puts it. A seed one column off
 * drops image column 0 and blanks the sprite's right edge, which a CRC alone
 * would not name. */
UTEST(mode4, affine_identity_matches_plain)
{
    int w, h, pw, ph;
    ASSERT_TRUE(corpus_size("mode4a_same", &w, &h));
    ASSERT_TRUE(corpus_size("mode4_same", &pw, &ph));
    ASSERT_EQ(w, pw);
    ASSERT_EQ(h, ph);
    const size_t px = (size_t)w * (size_t)h;

    char path[256];
    snprintf(path, sizeof(path), "%s/mode4a_same.rp6502", ROMS_DIR);
    ASSERT_TRUE(mut_boot(path));
    memcpy(settled, mut_frame(w, h), px * sizeof(uint32_t));

    snprintf(path, sizeof(path), "%s/mode4_same.rp6502", ROMS_DIR);
    ASSERT_TRUE(mut_boot(path));
    const uint32_t *plain = mut_frame(w, h);
    for (size_t i = 0; i < px; i++)
        if (settled[i] != plain[i])
        {
            fprintf(stderr, "affine/plain differ at %zu,%zu: %08X vs %08X\n",
                    i % (size_t)w, i / (size_t)w, settled[i], plain[i]);
            ASSERT_EQ(settled[i], plain[i]);
        }
}

UTEST(mode4, affine_rotate_scale_320x240)
{
    run_case(utest_result, "mode4a_rot", 0x3AB82A3C, MUT_BUDGET_NONE);
}

UTEST(mode4, affine_clips_over_fill_640x480)
{
    run_case(utest_result, "mode4a_clip", 0xE7650D4E, MUT_BUDGET_UNDER);
}

UTEST(mode4, log_range_halfword_descs_320x240)
{
    run_case(utest_result, "mode4_sizes", 0xBD73192F, MUT_BUDGET_UNDER);
}

UTEST(mode4, affine_small_and_rotated_largest_320x240)
{
    run_case(utest_result, "mode4a_sizes", 0x9B917685, MUT_BUDGET_NONE);
}

UTEST(mode4, odd_image_with_metadata_320x240)
{
    run_case(utest_result, "mode4_odd", 0x7E723099, MUT_BUDGET_NONE);
}

UTEST(mode4, affine_odd_image_320x240)
{
    run_case(utest_result, "mode4a_odd", 0x63349B0F, MUT_BUDGET_NONE);
}

UTEST(mode5, bpp8_8x8_sprite_only_320x240)
{
    run_case(utest_result, "mode5_8x8", 0xCB7FE557, MUT_BUDGET_NONE);
}

UTEST(mode5, bpp2_16x16_over_fill_320x240)
{
    run_case(utest_result, "mode5_16x16", 0x531D164B, MUT_BUDGET_NONE);
}

UTEST(mode5, bpp4_32x32_under_text_640x480)
{
    run_case(utest_result, "mode5_32x32", 0xE56BCD3B, MUT_BUDGET_UNDER);
}

UTEST(mode5, bpp8_64x64_plane1_320x180)
{
    run_case(utest_result, "mode5_64x64", 0xD6BAAF53, MUT_BUDGET_NONE);
}

UTEST(mode5, stress_budget_640x480)
{
    run_case(utest_result, "sprite_stress", 0xEE80425A, MUT_BUDGET_NONE);
}

UTEST(mode5, bpp1_128_halfword_descs_320x240)
{
    run_case(utest_result, "mode5_1bpp128", 0x1D403685, MUT_BUDGET_NONE);
}

UTEST(mode5, bpp4_256_640x480)
{
    run_case(utest_result, "mode5_4bpp256", 0xC38CE6F2, MUT_BUDGET_UNDER);
}

UTEST(mode5, one_sprite_on_its_row_320x240)
{
    run_case(utest_result, "mode5_onrow", 0x25D8DF03, MUT_BUDGET_UNDER);
}

UTEST(mode5, two_sprites_on_a_row_320x240)
{
    run_case(utest_result, "mode5_onrow2", 0x2BE392C6, MUT_BUDGET_UNDER);
}

UTEST(mode5, one_32x32_on_its_row_320x240)
{
    run_case(utest_result, "mode5_on32", 0x4657EB9D, MUT_BUDGET_UNDER);
}

UTEST(mode5, two_32x32_on_a_row_320x240)
{
    run_case(utest_result, "mode5_on32x2", 0x8923899F, MUT_BUDGET_UNDER);
}

UTEST(mode5, long_list_one_on_the_row_320x240)
{
    run_case(utest_result, "mode5_offrow", 0x25D8DF03, MUT_BUDGET_UNDER);
}

UTEST(mode4, one_sprite_on_its_row_320x240)
{
    run_case(utest_result, "mode4_onrow", 0x465A87A3, MUT_BUDGET_UNDER);
}

UTEST(mode4, two_sprites_on_a_row_320x240)
{
    run_case(utest_result, "mode4_onrow2", 0x2D69C35B, MUT_BUDGET_UNDER);
}

UTEST(mode4, one_8x8_on_its_row_320x240)
{
    run_case(utest_result, "mode4_on8", 0xAFB60952, MUT_BUDGET_UNDER);
}

UTEST(mode4, two_8x8_on_a_row_320x240)
{
    run_case(utest_result, "mode4_on8x2", 0x1F8FBC05, MUT_BUDGET_UNDER);
}

UTEST(mode4, two_affine_on_a_row_320x240)
{
    run_case(utest_result, "mode4a_onrow2", 0x2D69C35B, MUT_BUDGET_UNDER);
}

UTEST(mode4, long_affine_list_one_on_the_row_320x240)
{
    run_case(utest_result, "mode4a_offrow", 0x465A87A3, MUT_BUDGET_UNDER);
}

UTEST(mode4, long_list_one_on_the_row_320x240)
{
    run_case(utest_result, "mode4_offrow", 0x465A87A3, MUT_BUDGET_UNDER);
}

UTEST(mode0, overlay_windowed_640x480)
{
    run_case(utest_result, "mode0_overlay", 0x3AE0CA64, MUT_BUDGET_NONE);
}

UTEST(mode0, defaults_640x360)
{
    run_case(utest_result, "mode0_win360", 0x51B1630D, MUT_BUDGET_NONE);
}

UTEST(mode0, forty_column_320x240)
{
    run_case(utest_result, "mode0_win240", 0x0B292E81, MUT_BUDGET_NONE);
}

UTEST(mode0, forty_column_320x180)
{
    run_case(utest_result, "mode0_win180", 0x33775BC1, MUT_BUDGET_NONE);
}

UTEST(mode0, console_return_restores_vsync_line)
{
    run_case(utest_result, "mode0_return", 0x4D27B447, MUT_BUDGET_NONE);
}

UTEST(prog, bands_switch_modes_on_one_plane_320x240)
{
    run_case(utest_result, "prog_bands", 0xD2BCF38B, MUT_BUDGET_NONE);
}

/* 20 sprites of 64x64 at 8bpp stacked on one row of a 320 wide canvas. Their
 * images cycle through the palette, so the cache misses and a sprite costs
 * about a hundred clocks: more than one line, fewer than the two a 320 row
 * has. The bench measures the pair as one unit, so the printed worst is the
 * row's total against 3,198. */
UTEST(mode5, a_320_row_spends_two_lines_of_sprites)
{
    run_case(utest_result, "sprite_pair", 0x7C667FB3, MUT_BUDGET_UNDER);
}

MUT_MAIN()
