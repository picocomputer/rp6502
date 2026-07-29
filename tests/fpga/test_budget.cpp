/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the render actually spends. The engines race the beam: they fill
 * the next line's buffers while this one scans out, and the deadline is
 * pixel 799, which today is three thousand two hundred clocks after the
 * line began. Nothing has ever measured how much of that they use — the
 * only signal is the underrun assertion, which fires when the answer is
 * "all of it and more".
 *
 * So count. From each line's boundary to the clock where all three
 * planes and the sprite stage have gone idle, over the heaviest
 * fixtures the suite owns. The number decides whether the machine can
 * run at half the clock, where the timing analyser says every block but
 * the soft CPU closes: at 50.4 MHz the same line is sixteen hundred
 * clocks, and the engines would need to fit in that.
 *
 * This measures; it does not judge. The assertion is only against
 * today's deadline, so the test earns its place as an overrun guard
 * whatever we decide about the clock.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_quiet.h"
#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

#define PLANE_STATE(n) \
    dut->rootp->rp6502__DOT__gen_mode__BRA__##n##__KET____DOT__vid_mode__DOT__state

/* Clocks per scanline today: 800 pixels at four clocks each. */
static const long LINE_CLOCKS = 3200;

/* The sprite stage waits for every plane to finish before it plans
 * (vid_sprite.sv SP_WAIT), so a line is strictly the planes and then
 * the sprites. Worth knowing which half the clocks are in. */
struct budget_t
{
    long worst;        /* most clocks any one line took, end to end */
    int worst_line;
    long planes_at_worst;  /* of which, the planes' share */
    long sprite_at_worst;  /* and the sprite stage's */
    long worst_planes;     /* the planes' own worst, any line */
    /* Port A on the worst line: how many of those clocks actually
     * carried a word. Serialising the sprites behind the planes only
     * costs time if the port was idle while they waited. */
    long grants_at_worst;
    long grants_planes;    /* to requesters 0-2, the three fills */
    long grants_sprite;    /* to requester 3, the sprite stage */
    long lines;
};

static void clock_cycle()
{
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_sys = 0;
    dut->eval();
}

static bool render_idle()
{
    return PLANE_STATE(0) == 0 && PLANE_STATE(1) == 0 && PLANE_STATE(2) == 0
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
    b->lines = 0;

    /* Start at a line boundary so the first count is whole. */
    uint16_t prev = dut->rp6502_scanline;
    while (dut->rp6502_scanline == prev)
        clock_cycle();

    for (int line = 0; line < 525; line++)
    {
        prev = dut->rp6502_scanline;
        long clocks = 0, busy_until = 0, planes_until = 0;
        long grants = 0, g_planes = 0, g_sprite = 0;
        while (dut->rp6502_scanline == prev)
        {
            clock_cycle();
            clocks++;
            if (!render_idle())
                busy_until = clocks;
            if (PLANE_STATE(0) != 0 || PLANE_STATE(1) != 0
                || PLANE_STATE(2) != 0)
                planes_until = clocks;
            if (dut->rootp->rp6502__DOT__a_any)
            {
                grants++;
                unsigned sel = dut->rootp->rp6502__DOT__a_sel;
                if (sel < 3)
                    g_planes++;
                else if (sel == 3)
                    g_sprite++;
            }
        }
        b->lines++;
        if (planes_until > b->worst_planes)
            b->worst_planes = planes_until;
        if (busy_until > b->worst)
        {
            b->worst = busy_until;
            b->worst_line = (int)prev;
            b->planes_at_worst = planes_until;
            b->sprite_at_worst = busy_until - planes_until;
            b->grants_at_worst = grants;
            b->grants_planes = g_planes;
            b->grants_sprite = g_sprite;
        }
    }
}

static void run_case(int *utest_result, const char *name)
{
    std::string path = std::string(ROMS_DIR "/") + name + ".rp6502";
    FILE *f = fopen(path.c_str(), "rb");
    ASSERT_TRUE(f != NULL);
    std::vector<uint8_t> rom;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);
    ASSERT_TRUE(rom.size() > 0);

    auto *r = dut->rootp;
    ASSERT_TRUE(tb_load_tcm(r->rp6502__DOT__rv__DOT__tcm0,
                            r->rp6502__DOT__rv__DOT__tcm1,
                            r->rp6502__DOT__rv__DOT__tcm2,
                            r->rp6502__DOT__rv__DOT__tcm3, SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    r->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();
    ASSERT_TRUE(tb_quiet(dut, [&] {
        dut->stage_rdata = tb_stage(rom, dut->rp6502_stage_addr);
        clock_cycle();
    }));

    /* Two frames: the first can begin mid-line, the second is clean. */
    budget_t b;
    measure_frame(&b);
    measure_frame(&b);

    printf("  %-18s worst %4ld of %ld (%2ld%%) on line %3d"
           "  =  planes %4ld + sprites %4ld"
           "   |  at half the clock %4ld/1600 = %3ld%%%s\n",
           name, b.worst, LINE_CLOCKS, b.worst * 100 / LINE_CLOCKS,
           b.worst_line, b.planes_at_worst, b.sprite_at_worst,
           b.worst, b.worst * 100 / 1600,
           b.worst > 1600 ? "  OVER" : "");
    printf("  %-18s   port A carried %4ld words in those %4ld clocks"
           " (%2ld%% busy) — planes %4ld, sprites %4ld\n",
           name, b.grants_at_worst, b.worst,
           b.worst ? b.grants_at_worst * 100 / b.worst : 0,
           b.grants_planes, b.grants_sprite);
    fflush(stdout);

    ASSERT_EQ(b.lines, 525);
    /* Today's deadline. The engines assert on their own underrun, so
     * this is belt and braces — but it is the number this test is
     * really about, and it should never silently creep. */
    ASSERT_LT(b.worst, LINE_CLOCKS);
}

UTEST(budget, sprite_stress_the_documented_budget)
{
    run_case(utest_result, "sprite_stress");
}

UTEST(budget, mode4_cross_plane_640x480) { run_case(utest_result, "mode4_32"); }
UTEST(budget, mode5_4bpp_256_640x480) { run_case(utest_result, "mode5_4bpp256"); }
UTEST(budget, mode3_8bpp_640x480) { run_case(utest_result, "mode3_8bpp"); }
UTEST(budget, mode1_16bpp_8x16) { run_case(utest_result, "mode1_16bpp8x16"); }
UTEST(budget, mode2_composite) { run_case(utest_result, "mode2_composite"); }

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    oracle_init();
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
