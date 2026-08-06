/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Every video mode against the oracle, one fixture at a time. The ROMs
 * come from tests/roms — the corpus vidmodes.py generates and the
 * emulator suite also boots — so both machines run the identical file
 * and settle. The oracle renders canvas-native; the RTL does the
 * doubling and letterboxing itself, and its de is the canvas, so a
 * captured frame is exactly canvas-many pixels and the two framebuffers
 * compare word for word.
 *
 * One file for five modes because there was never five of anything: the
 * suites were the same hundred lines with a different list of fixture
 * names, and the copies had already drifted — one of them capped its
 * debug output at twelve differences and the rest at twenty. The claim
 * a mode test makes is the same claim for all of them. What differs is
 * which fixtures exercise it, and that is the list below.
 *
 * The cases keep their suite names, so ctest still names them mode1..5
 * and a run picks them apart the way it always did.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

/* One fixture end to end: run both machines, settle, compare. The
 * second capture is not redundant — it is what says the picture is
 * settled rather than merely correct once. */
static void run_case(int *utest_result, const char *name)
{
    std::string path = std::string(ROMS_DIR "/") + name + ".rp6502";
    std::vector<uint8_t> rom;
    ASSERT_TRUE(tb_rom_read(path.c_str(), rom));

    ASSERT_TRUE(oracle_restart(path.c_str()));
    oracle_run_frames(30);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    const uint32_t *ofb = oracle_framebuffer();

    ASSERT_TRUE(tb_boot(dut, rom));

    static uint32_t fb[2][640 * 480];
    const size_t px = (size_t)ow * (size_t)oh;
    tb_capture(dut, fb[0], px);
    tb_capture(dut, fb[1], px);
    ASSERT_EQ(memcmp(fb[0], fb[1], px * 4), 0);

    int diffs = 0;
    for (int y = 0; y < oh; y++)
        for (int x = 0; x < ow; x++)
        {
            uint32_t want = ofb[(size_t)y * ow + x];
            if (fb[0][(size_t)y * ow + x] != want)
            {
                if (getenv("MODES_DEBUG") && diffs < 20)
                    fprintf(stderr, "%s diff x=%d y=%d rtl=%08X want=%08X\n",
                            name, x, y, fb[0][(size_t)y * ow + x], want);
                diffs++;
            }
        }
    ASSERT_EQ(diffs, 0);
}

/* Mode 1 character cells, one format at a time: bare glyphs, packed and
 * reversed nibble colors, byte-indexed colors, raw sixteen-bit colors —
 * through both font heights, the builtin ROM and an XRAM font, builtin
 * and XRAM palettes. */

UTEST(mode1, bpp1_8x8_builtin_640x480)
{
    run_case(utest_result, "mode1_1bpp8x8");
}

UTEST(mode1, bpp4_8x16_xram_palette_320x240)
{
    run_case(utest_result, "mode1_4bpp8x16");
}

UTEST(mode1, bpp4r_8x8_builtin_320x180)
{
    run_case(utest_result, "mode1_4bppr8x8");
}

UTEST(mode1, bpp8_8x8_xram_font_320x240)
{
    run_case(utest_result, "mode1_8bpp8x8");
}

UTEST(mode1, bpp16_8x16_640x360)
{
    run_case(utest_result, "mode1_16bpp8x16");
}

UTEST(mode1, bpp1_wrap_halfword_palette_320x240)
{
    run_case(utest_result, "mode1_wrap");
}

/* Mode 2 tile maps: every depth, both tile sizes, trimmed tiles,
 * wraparound, builtin and XRAM palettes — and the composite case
 * stacking a mode 3 base under mode 2 tiles and mode 1 cells across all
 * three planes. */

UTEST(mode2, bpp1_8px_builtin_640x480)
{
    run_case(utest_result, "mode2_1bpp8");
}

UTEST(mode2, bpp2_16px_xram_palette_320x240)
{
    run_case(utest_result, "mode2_2bpp16");
}

UTEST(mode2, bpp4_8px_trimmed_320x240)
{
    run_case(utest_result, "mode2_4bpp8trim");
}

UTEST(mode2, bpp8_16px_wrap_320x180)
{
    run_case(utest_result, "mode2_8bpp16wrap");
}

UTEST(mode2, composite_three_planes_320x240)
{
    run_case(utest_result, "mode2_composite");
}

UTEST(mode2, bpp2_16px_both_trims_320x240)
{
    run_case(utest_result, "mode2_16trim");
}

UTEST(mode2, bpp4_8px_xtrim_640x480)
{
    run_case(utest_result, "mode2_trimx");
}

UTEST(mode2, bpp8_8px_xtrim_320x180)
{
    run_case(utest_result, "mode2_trimx8");
}

UTEST(mode2, bpp1_8px_ytrim_320x180)
{
    run_case(utest_result, "mode2_trimy");
}

/* Mode 3 bitmaps, one canvas geometry and depth at a time. */

UTEST(mode3, bpp8_xram_palette_640x480)
{
    run_case(utest_result, "mode3_8bpp");
}

UTEST(mode3, bpp1_builtin_320x240)
{
    run_case(utest_result, "mode3_1bpp");
}

UTEST(mode3, bpp4_reversed_320x180)
{
    run_case(utest_result, "mode3_4bppr");
}

UTEST(mode3, bpp16_640x360)
{
    run_case(utest_result, "mode3_16bpp");
}

UTEST(mode3, bpp2_halfword_ptrs_320x240)
{
    run_case(utest_result, "mode3_2bpp");
}

UTEST(mode3, bpp4_builtin_640x480)
{
    run_case(utest_result, "mode3_4bpp");
}

UTEST(mode3, bpp1_reversed_320x180)
{
    run_case(utest_result, "mode3_1bppr");
}

UTEST(mode3, bpp2_reversed_lower_640x360)
{
    run_case(utest_result, "mode3_2bppr");
}

UTEST(mode3, bpp8_wrap_bound_ptrs_320x240)
{
    run_case(utest_result, "mode3_wrap");
}

/* Mode 4 sprites: raw sixteen-bit squares with alpha-gated texels and
 * opacity metadata — narrowed sparse rows and continuous full rows —
 * from sprite-only, over-fill, and cross-plane slots, clipped off every
 * edge. */

UTEST(mode4, log3_sprite_only_320x240)
{
    run_case(utest_result, "mode4_8");
}

UTEST(mode4, log4_metadata_over_fill_320x240)
{
    run_case(utest_result, "mode4_meta16");
}

UTEST(mode4, log5_cross_plane_640x480)
{
    run_case(utest_result, "mode4_32");
}

UTEST(mode4, log6_plane2_320x180)
{
    run_case(utest_result, "mode4_64");
}

UTEST(mode4, affine_identity_320x240)
{
    run_case(utest_result, "mode4a_id");
}

UTEST(mode4, affine_rotate_scale_320x240)
{
    run_case(utest_result, "mode4a_rot");
}

UTEST(mode4, affine_clips_over_fill_640x480)
{
    run_case(utest_result, "mode4a_clip");
}

UTEST(mode4, log_range_halfword_descs_320x240)
{
    run_case(utest_result, "mode4_sizes");
}

UTEST(mode4, affine_small_and_large_320x240)
{
    run_case(utest_result, "mode4a_sizes");
}

/* Mode 5 sprites: a sprite-only plane claiming a zeroed layer, sprites
 * over a fill on their own plane, sprites under a text plane above, and
 * the big squares from a non-zero plane on the letterboxed canvas —
 * clips off every edge, overlap, per-sprite palettes with the builtin
 * fallback and a halfword-aligned read. */

UTEST(mode5, bpp8_8x8_sprite_only_320x240)
{
    run_case(utest_result, "mode5_8x8");
}

UTEST(mode5, bpp2_16x16_over_fill_320x240)
{
    run_case(utest_result, "mode5_16x16");
}

UTEST(mode5, bpp4_32x32_under_text_640x480)
{
    run_case(utest_result, "mode5_32x32");
}

UTEST(mode5, bpp8_64x64_plane1_320x180)
{
    run_case(utest_result, "mode5_64x64");
}

UTEST(mode5, stress_budget_640x480)
{
    run_case(utest_result, "sprite_stress");
}

UTEST(mode5, bpp1_128_halfword_descs_320x240)
{
    run_case(utest_result, "mode5_1bpp128");
}

UTEST(mode5, bpp4_256_640x480)
{
    run_case(utest_result, "mode5_4bpp256");
}

/* The one case with no oracle: more sprites than the engine can fetch,
 * where what is asserted is that it says so and still paints the same
 * picture twice. */
UTEST(mode5, overrun_counts_lost_races_320x240)
{
    std::vector<uint8_t> rom;
    ASSERT_TRUE(tb_rom_read(ROMS_DIR "/sprite_overrun.rp6502", rom));
    ASSERT_TRUE(tb_boot(dut, rom));

    /* sprite_overrun is built on canvas 1 — 320x240, vidmodes.py — and
     * this case runs no oracle, so the frame size is stated here. */
    static uint32_t fb[2][640 * 480];
    const size_t px = 320 * 240;
    tb_capture(dut, fb[0], px);
    uint16_t over = dut->rootp->rp6502__DOT__vid_sprite__DOT__vid_sprite_overrun;
    tb_capture(dut, fb[1], px);
    ASSERT_EQ(memcmp(fb[0], fb[1], px * 4), 0);
    ASSERT_GT(over, 0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    oracle_init();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
