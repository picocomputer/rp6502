/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Every video mode, on whichever machine this tree builds.
 *
 * The ROMs are the corpus vidmodes.py generates; the manifest beside them
 * states each one's canvas. A case boots its ROM, waits for the picture to
 * settle, and checks the frame against the CRC written down beside it here.
 *
 * That expectation is the point. This suite used to exist twice — once
 * asserting the emulator drew something and settled, once rendering it in the
 * fabric and diffing against the emulator running inside the same test. The
 * second shape came from developing the RTL against a C oracle, and it left
 * neither machine able to be tested without the other. Neither is the oracle
 * now: both render, and both answer to the number in the case.
 *
 * Re-blessing a deliberate renderer change is editing the case that failed,
 * so the expectation and the claim it belongs to move in one diff. Set
 * RP6502_BLESS_CRC to have a run print every case's observed value in the
 * form it is pasted back as.
 *
 * The render budget rides along for the fixtures heavy enough to be worth
 * asking, because those are these fixtures and a second suite over them was a
 * second boot. A machine with no beam answers MUT_BUDGET_NONE and the claim
 * is skipped rather than invented.
 */

#include "corpus.h"
#include "host/host.h"
#include "mut.h"
#include "utest.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t settled[640 * 480];

/* One fixture end to end. The second frame is not redundant — it is what says
 * the picture settled rather than merely arrived once. */
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

/* Mode 1 character cells, one format at a time: bare glyphs, packed and
 * reversed nibble colors, byte-indexed colors, raw sixteen-bit colors —
 * through both font heights, the builtin ROM and an XRAM font, builtin
 * and XRAM palettes. */

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

/* Mode 2 tile maps: every depth, both tile sizes, trimmed tiles,
 * wraparound, builtin and XRAM palettes — and the composite case
 * stacking a mode 3 base under mode 2 tiles and mode 1 cells across all
 * three planes. */

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

/* Mode 3 bitmaps, one canvas geometry and depth at a time. */

UTEST(mode3, bpp8_xram_palette_640x480)
{
    run_case(utest_result, "mode3_8bpp", 0x6B55D171, MUT_BUDGET_UNDER);
}

/* The serial canary: the wide canvas's two fills, both with the 8bpp
 * XRAM-palette prologue, back to back on the one engine — the tightest
 * legal line the machine can be asked for. */

UTEST(mode3, two_bpp8_fills_serial_640x480)
{
    run_case(utest_result, "fill_heavy640", 0x42E2D810, MUT_BUDGET_UNDER);
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

/* Mode 4 sprites: raw sixteen-bit squares with alpha-gated texels and
 * opacity metadata — narrowed sparse rows and continuous full rows —
 * from sprite-only, over-fill, and cross-plane slots, clipped off every
 * edge. */

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
    run_case(utest_result, "mode4a_id", 0x8BF4D7AA, MUT_BUDGET_NONE);
}

UTEST(mode4, affine_rotate_scale_320x240)
{
    run_case(utest_result, "mode4a_rot", 0x3A9DC1C9, MUT_BUDGET_NONE);
}

UTEST(mode4, affine_clips_over_fill_640x480)
{
    run_case(utest_result, "mode4a_clip", 0x960EBF4F, MUT_BUDGET_UNDER);
}

UTEST(mode4, log_range_halfword_descs_320x240)
{
    run_case(utest_result, "mode4_sizes", 0x6826275A, MUT_BUDGET_UNDER);
}

UTEST(mode4, affine_small_and_large_320x240)
{
    run_case(utest_result, "mode4a_sizes", 0xBFD3E7B6, MUT_BUDGET_NONE);
}

/* Mode 5 sprites: a sprite-only plane claiming a zeroed layer, sprites
 * over a fill on their own plane, sprites under a text plane above, and
 * the big squares from a non-zero plane on the 320x180 canvas —
 * clips off every edge, overlap, per-sprite palettes with the builtin
 * fallback and a halfword-aligned read. */

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

/* Mode 0 as a slot: the terminal over a mode-3 bitmap on plane 1 —
 * default-background cells transparent, inked cells opaque — pinned
 * pixel-exact on every canvas geometry. win240 and
 * win180 walk the 40-column 8x8 path, DEC graphics included. */

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

/* The console canvas coming back after a mode-0 slot returns to it. The
 * fabric's own vsync shadow is checked in tests/rtl/vga, which is the only
 * machine that has one. */
UTEST(mode0, console_return_restores_vsync_line)
{
    run_case(utest_result, "mode0_return", 0x4D27B447, MUT_BUDGET_NONE);
}

MUT_MAIN()
