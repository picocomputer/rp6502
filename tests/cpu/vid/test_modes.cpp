/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Every video mode against the oracle, one fixture at a time. The ROMs
 * come from tests/roms — the corpus vidmodes.py generates and the
 * emulator suite also boots — so both machines run the identical file
 * and settle. Both machines render canvas-native and the RTL's de is
 * the canvas, so a captured frame is exactly canvas-many pixels and
 * the two framebuffers compare word for word.
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
 *
 * The render budget is measured here too, for the fixtures heavy enough
 * to be worth asking. It used to be its own suite over its own boots,
 * and every one of its ten fixtures is one of these — so the machine
 * was loading each of them twice to make two claims about one frame.
 * The claims are still two; the boot is now one. What the numbers mean,
 * and why they are a floor rather than a budget, is under measure_frame.
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

#define FILL_STATE dut->rootp->rp6502__DOT__vid_fill__DOT__state
#define SCHED_STATE dut->rootp->rp6502__DOT__vid_sched__DOT__state
#define SCHED_PENDING dut->rootp->rp6502__DOT__vid_sched__DOT__plane_pending

/* A scanline is 800 pixels at two clocks; the deadline is the last of
 * them, not the end of the line. */
static const long LINE_CLOCKS = 1600;
static const long LINE_DEADLINE = 2 * 799;

/* The sprite stage owns its three line buffers and never waits on a
 * fill or clears a bank (the buffers erase themselves behind the beam),
 * so it runs the whole line concurrent with the planes. The two shares
 * below overlap; they only couple through port A, where sprite fetches
 * now contend with fills instead of following them. */
struct budget_t
{
    long worst;        /* most clocks any one line took, end to end */
    int worst_line;
    long planes_at_worst;  /* the planes' own finish on that line */
    long sprite_at_worst;  /* the sprite stage's, concurrent not added */
    long worst_planes;     /* the planes' own worst, any line */
    /* Port A on the worst line: how many of those clocks actually
     * carried a word, and whose it was. */
    long grants_at_worst;
    long grants_planes;    /* to requester 0, the one fill engine */
    long grants_sprite;    /* to requester 1, the sprite stage */
    /* The terminal renders every line whatever the canvas — vid_mode0
     * raises run at every line_start — and its cost is concurrent, not
     * added. It still has to fit the line on its own. */
    long worst_term;
    /* Where the line's clocks go: each plane's resolution — its fill's
     * finish on the shared engine, or the decision that skipped it —
     * and the sprite stage's time by state. SP_IDLE=0 SLOT=1 PLAN=2
     * RUN=3. */
    long plane_done[3];
    long sp_state[4];
    long lines;
};

static bool render_idle()
{
    return SCHED_STATE == 0 && FILL_STATE == 0
           && dut->rootp->rp6502__DOT__vid_sprite__DOT__state == 0;
}

/* One frame, watching every line. A line's cost is the clock at which
 * the last engine went idle, counted from the boundary — the engines
 * all start together at line_start, so that is the whole of it. */
static void measure_frame(budget_t *b)
{
    b->worst = 0;
    b->worst_line = -1;
    b->planes_at_worst = 0;
    b->sprite_at_worst = 0;
    b->worst_planes = 0;
    b->grants_at_worst = 0;
    b->grants_planes = 0;
    b->grants_sprite = 0;
    b->worst_term = 0;
    for (int i = 0; i < 3; i++)
        b->plane_done[i] = 0;
    for (int i = 0; i < 4; i++)
        b->sp_state[i] = 0;
    b->lines = 0;

    /* Start at a line boundary so the first count is whole. */
    uint16_t prev = dut->rp6502_scanline;
    while (dut->rp6502_scanline == prev)
        tb_clock(dut);

    for (int line = 0; line < 525; line++)
    {
        prev = dut->rp6502_scanline;
        long clocks = 0, busy_until = 0, planes_until = 0, sprite_until = 0;
        long grants = 0, g_planes = 0, g_sprite = 0, term_until = 0;
        long pdone[3] = {0, 0, 0};
        long spst[4] = {0, 0, 0, 0};
        while (dut->rp6502_scanline == prev)
        {
            tb_clock(dut);
            clocks++;
            if (!render_idle())
                busy_until = clocks;
            if (SCHED_STATE != 0)
                planes_until = clocks;
            if (SCHED_PENDING & 1) pdone[0] = clocks;
            if (SCHED_PENDING & 2) pdone[1] = clocks;
            if (SCHED_PENDING & 4) pdone[2] = clocks;
            if (dut->rootp->rp6502__DOT__vid_sprite__DOT__state != 0)
                sprite_until = clocks;
            spst[dut->rootp->rp6502__DOT__vid_sprite__DOT__state & 3]++;
            if (dut->rootp->rp6502__DOT__vid_mode0__DOT__run)
                term_until = clocks;
            if (dut->rootp->rp6502__DOT__a_any)
            {
                grants++;
                unsigned sel = dut->rootp->rp6502__DOT__a_sel;
                if (sel == 0)
                    g_planes++;
                else if (sel == 1)
                    g_sprite++;
            }
        }
        b->lines++;
        if (term_until > b->worst_term)
            b->worst_term = term_until;
        if (planes_until > b->worst_planes)
            b->worst_planes = planes_until;
        if (busy_until > b->worst)
        {
            b->worst = busy_until;
            b->worst_line = (int)prev;
            b->planes_at_worst = planes_until;
            b->sprite_at_worst = sprite_until;
            b->grants_at_worst = grants;
            b->grants_planes = g_planes;
            b->grants_sprite = g_sprite;
            for (int i = 0; i < 3; i++)
                b->plane_done[i] = pdone[i];
            for (int i = 0; i < 4; i++)
                b->sp_state[i] = spst[i];
        }
    }
}

/* What a fixture is asked to prove about the render's timing, on top of
 * its pixels. */
enum budget_claim
{
    budget_none,   /* not one of the heavy fixtures */
    budget_under,  /* a real program's worth of work, inside the beam */
    budget_report, /* deliberately past it; measured, not gated */
    budget_over,   /* built to lose the race, and must be seen to */
};

/* The frame the budget is taken from is the settled one, which is why
 * this runs after the captures rather than off the back of the run. */
static void measure(int *utest_result, const char *name, budget_claim claim)
{
    if (claim == budget_none)
        return;
    budget_t b;
    measure_frame(&b);
    printf("  %-18s worst %4ld of %ld (%2ld%%) on line %3d"
           "  =  planes %4ld, sprites %4ld, concurrent"
           "   |  against the deadline %4ld/%ld = %3ld%%%s\n",
           name, b.worst, LINE_CLOCKS, b.worst * 100 / LINE_CLOCKS,
           b.worst_line, b.planes_at_worst, b.sprite_at_worst,
           b.worst, LINE_DEADLINE, b.worst * 100 / LINE_DEADLINE,
           b.worst >= LINE_DEADLINE ? "  OVER" : "");
    printf("  %-18s   port A carried %4ld words in those %4ld clocks"
           " (%2ld%% busy) — planes %4ld, sprites %4ld\n",
           name, b.grants_at_worst, b.worst,
           b.worst ? b.grants_at_worst * 100 / b.worst : 0,
           b.grants_planes, b.grants_sprite);
    printf("  %-18s   terminal %4ld clocks a line, every line, "
           "concurrent with all of it\n", name, b.worst_term);
    printf("  %-18s   planes done at %4ld %4ld %4ld  |  sprite stage: "
           "slot %3ld plan %3ld run %4ld\n",
           name, b.plane_done[0], b.plane_done[1], b.plane_done[2],
           b.sp_state[1], b.sp_state[2], b.sp_state[3]);
    fflush(stdout);

    ASSERT_EQ(b.lines, 525);
    if (claim == budget_under)
        ASSERT_LT(b.worst, LINE_DEADLINE);
    if (claim == budget_over)
    {
        ASSERT_GE(b.worst, LINE_DEADLINE);
        ASSERT_GT(dut->rootp->rp6502__DOT__vid_sprite__DOT__vid_sprite_overrun,
                  0);
    }
}

/* One fixture end to end: run both machines, settle, compare. The
 * second capture is not redundant — it is what says the picture is
 * settled rather than merely correct once. */
static void run_case(int *utest_result, const char *name,
                     budget_claim claim = budget_none)
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

    measure(utest_result, name, claim);
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
    run_case(utest_result, "mode1_16bpp8x16", budget_under);
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
    run_case(utest_result, "mode2_composite", budget_under);
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
    run_case(utest_result, "mode3_8bpp", budget_under);
}

/* The serial canary: the wide canvas's two fills, both with the 8bpp
 * XRAM-palette prologue, back to back on the one engine — the tightest
 * legal line the machine can be asked for. */
UTEST(mode3, two_bpp8_fills_serial_640x480)
{
    run_case(utest_result, "fill_heavy640", budget_under);
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
    run_case(utest_result, "mode4_32", budget_under);
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
    run_case(utest_result, "mode4a_clip", budget_under);
}

UTEST(mode4, log_range_halfword_descs_320x240)
{
    run_case(utest_result, "mode4_sizes", budget_under);
}

UTEST(mode4, affine_small_and_large_320x240)
{
    run_case(utest_result, "mode4a_sizes");
}

/* Mode 5 sprites: a sprite-only plane claiming a zeroed layer, sprites
 * over a fill on their own plane, sprites under a text plane above, and
 * the big squares from a non-zero plane on the 320x180 canvas —
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
    run_case(utest_result, "mode5_32x32", budget_under);
}

UTEST(mode5, bpp8_64x64_plane1_320x180)
{
    run_case(utest_result, "mode5_64x64");
}

UTEST(mode5, stress_budget_640x480)
{
    run_case(utest_result, "sprite_stress", budget_report);
}

UTEST(mode5, bpp1_128_halfword_descs_320x240)
{
    run_case(utest_result, "mode5_1bpp128");
}

UTEST(mode5, bpp4_256_640x480)
{
    run_case(utest_result, "mode5_4bpp256", budget_under);
}

/* Mode 0 as a slot: the terminal over a mode-3 bitmap on plane 1 —
 * default-background cells transparent, inked cells opaque — proven
 * pixel-exact against the oracle on every canvas geometry. win240 and
 * win180 walk the 40-column 8x8 path, DEC graphics included. */

UTEST(mode0, overlay_windowed_640x480)
{
    run_case(utest_result, "mode0_overlay");
}

UTEST(mode0, defaults_640x360)
{
    run_case(utest_result, "mode0_win360");
}

UTEST(mode0, forty_column_320x240)
{
    run_case(utest_result, "mode0_win240");
}

UTEST(mode0, forty_column_320x180)
{
    run_case(utest_result, "mode0_win180");
}

UTEST(mode0, console_return_restores_vsync_line)
{
    run_case(utest_result, "mode0_return");
    ASSERT_EQ(dut->rootp->rp6502__DOT__vid_prog__DOT__vsync_shadow, 480);
}

/* The one case with no oracle: more sprites than the engine can fetch,
 * where what is asserted is that it says so and still paints the same
 * picture twice — and that the line it lost is measurably past the
 * beam's deadline, which is the fixture's whole reason for existing. */
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

    measure(utest_result, "sprite_overrun", budget_over);
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
