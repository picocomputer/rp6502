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
static uint32_t trial[640 * 480];

/* A frame is taken once two in a row agree rather than at a fixed distance
 * from the boot, because the console goes on redrawing for a few frames after
 * the last byte reaches it. A scene that is already still costs two frames. */
static bool settle(uint32_t *dst, int w, int h)
{
    const size_t bytes = (size_t)w * (size_t)h * sizeof(uint32_t);
    const uint32_t *frame = mut_frame(w, h);
    for (int i = 0; i < 8; i++)
    {
        memcpy(dst, frame, bytes);
        frame = mut_frame(w, h);
        if (memcmp(dst, frame, bytes) == 0)
            return true;
    }
    return false;
}

static void run_case(int *utest_result, const char *name, uint32_t expect,
                     mut_budget_t claim)
{
    int w, h;
    ASSERT_TRUE(corpus_size(name, &w, &h));
    const size_t px = (size_t)w * (size_t)h;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.rp6502", ROMS_DIR, name);
    ASSERT_TRUE(mut_boot(path));

    ASSERT_TRUE(settle(settled, w, h));

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

/* The reference frame is pinned by CRC and the frame under test has to match
 * it pixel for pixel, which names the first pixel that is off. run_case leaves
 * the reference's frame in settled, so only the frame under test boots here. */
static void run_pair(int *utest_result, const char *name, const char *ref,
                     uint32_t ref_crc)
{
    run_case(utest_result, ref, ref_crc, MUT_BUDGET_NONE);
    if (*utest_result != UTEST_TEST_PASSED)
        return;
    int w, h, nw, nh;
    ASSERT_TRUE(corpus_size(ref, &w, &h));
    ASSERT_TRUE(corpus_size(name, &nw, &nh));
    ASSERT_EQ(w, nw);
    ASSERT_EQ(h, nh);
    const size_t px = (size_t)w * (size_t)h;

    char path[256];
    snprintf(path, sizeof(path), "%s/%s.rp6502", ROMS_DIR, name);
    ASSERT_TRUE(mut_boot(path));
    ASSERT_TRUE(settle(trial, w, h));
    for (size_t i = 0; i < px; i++)
        if (trial[i] != settled[i])
        {
            fprintf(stderr, "%s/%s differ at %zu,%zu: %08X vs %08X\n",
                    name, ref, i % (size_t)w, i / (size_t)w, trial[i], settled[i]);
            ASSERT_EQ(trial[i], settled[i]);
        }
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
 * would not name. mode4_same has mode4a_id's CRC: mode4a_same is that fixture
 * again, and the plain frame matches it. */
UTEST(mode4, affine_identity_matches_plain)
{
    run_pair(utest_result, "mode4a_same", "mode4_same", 0x37028F77);
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

/* Custom mode 5 sprites. A pair's reference draws the same picture from
 * images transformed by the generator, with the sprite flags clear. */
UTEST(mode5c, seven_depths_and_widths_320x240)
{
    run_case(utest_result, "mode5c_depths", 0x070C9FE9, MUT_BUDGET_NONE);
}

UTEST(mode5c, hflip_matches_flipped_images_320x240)
{
    run_pair(utest_result, "mode5c_hflip", "mode5c_hflip_ref", 0x1906479F);
}

UTEST(mode5c, vflip_matches_flipped_images_320x240)
{
    run_pair(utest_result, "mode5c_vflip", "mode5c_vflip_ref", 0x08C28F14);
}

UTEST(mode5c, hdbl_matches_doubled_images_320x240)
{
    run_pair(utest_result, "mode5c_hdbl", "mode5c_hdbl_ref", 0xA8158FB6);
}

UTEST(mode5c, vdbl_matches_doubled_images_320x240)
{
    run_pair(utest_result, "mode5c_vdbl", "mode5c_vdbl_ref", 0xB3BCB9D4);
}

UTEST(mode5c, all_four_flags_match_transformed_images_320x240)
{
    run_pair(utest_result, "mode5c_all4", "mode5c_all4_ref", 0x69581384);
}

UTEST(mode5c, clipped_at_every_edge_with_every_flag_320x240)
{
    run_pair(utest_result, "mode5c_clip", "mode5c_clip_ref", 0xB1434199);
}

UTEST(mode5c, doubled_at_the_right_edge_over_fill_640x480)
{
    run_pair(utest_result, "mode5c_clip640", "mode5c_clip640_ref",
             0x8860D382);
}

UTEST(mode5c, mixed_depths_and_flags_320x240)
{
    run_case(utest_result, "mode5c_half_even", 0x64113D45, MUT_BUDGET_NONE);
}

/* mode5c_half is mode5c_half_even with its descriptors at the halfword
 * address $0102, so the two frames are the same. */
UTEST(mode5c, halfword_descs_match_word_descs_320x240)
{
    run_pair(utest_result, "mode5c_half", "mode5c_half_even", 0x64113D45);
}

UTEST(mode5c, palette_pointers_at_the_limit_of_each_depth_320x240)
{
    run_case(utest_result, "mode5c_pal", 0xCDBD8BDC, MUT_BUDGET_NONE);
}

UTEST(mode5c, smallest_largest_and_doubled_sizes_320x240)
{
    run_case(utest_result, "mode5c_size", 0x32BA5443, MUT_BUDGET_NONE);
}

/* The same 16x16 4bpp scene in the fixed and the custom form: the images are
 * the same bytes at the same addresses. */
UTEST(mode5c, custom_matches_fixed_size_320x240)
{
    run_pair(utest_result, "mode5c_same", "mode5_same", 0xF439C3C2);
}

UTEST(mode5c, custom_plane_over_fixed_plane_320x240)
{
    run_case(utest_result, "mode5c_planes", 0x81454F57, MUT_BUDGET_NONE);
}

UTEST(mode5c, eleven_sprites_a_row_320x240)
{
    run_case(utest_result, "mode5c_full", 0xADF88D47, MUT_BUDGET_NONE);
}

/* mode5c_full2 is mode5c_full with its descriptors at $0102. */
UTEST(mode5c, eleven_sprites_a_row_halfword_descs_320x240)
{
    run_pair(utest_result, "mode5c_full2", "mode5c_full", 0xADF88D47);
}

UTEST(mode5c, one_32x32_on_its_row_320x240)
{
    run_case(utest_result, "mode5c_on32", 0x333F4807, MUT_BUDGET_UNDER);
}

UTEST(mode5c, one_doubled_16x32_on_its_row_320x240)
{
    run_case(utest_result, "mode5c_hdbl_on32", 0x92390CBE, MUT_BUDGET_UNDER);
}

UTEST(mode5c, one_flipped_32x32_on_its_row_320x240)
{
    run_case(utest_result, "mode5c_hflip_on32", 0x106DC6FF,
             MUT_BUDGET_UNDER);
}

UTEST(mode5c, two_32x32_on_a_row_320x240)
{
    run_case(utest_result, "mode5c_onrow2", 0x3E98F993, MUT_BUDGET_UNDER);
}

/* One drawn sprite alone, and the same sprite behind a list of two hundred
 * that are off the row: the pair measures what an entry costs to walk. */
UTEST(mode5c, one_16x16_on_its_row_320x240)
{
    run_case(utest_result, "mode5c_onrow", 0xAFEE1EAB, MUT_BUDGET_UNDER);
}

UTEST(mode5c, long_list_one_on_the_row_320x240)
{
    run_case(utest_result, "mode5c_offrow", 0xAFEE1EAB, MUT_BUDGET_UNDER);
}

UTEST(mode5c, stack_of_flipped_and_doubled_320x240)
{
    run_case(utest_result, "mode5c_stack", 0x3CE4296B, MUT_BUDGET_UNDER);
}

/* Forty doubled 32x8 8bpp sprites on one row, each walking the whole palette:
 * more than one line's clocks and fewer than the two a 320 row has. */
UTEST(mode5c, a_320_row_spends_two_lines_of_doubled_sprites)
{
    run_case(utest_result, "mode5c_pair", 0xD7941DC1, MUT_BUDGET_UNDER);
}

MUT_MAIN()
