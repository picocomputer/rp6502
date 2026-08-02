/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What the render actually spends. The engines race the beam: they fill
 * the next line's buffers while this one scans out, and the deadline is
 * pixel 799, which today is sixteen hundred clocks after the
 * line began. Nothing has ever measured how much of that they use — the
 * only signal is the underrun assertion, which fires when the answer is
 * "all of it and more".
 *
 * So count. From each line's boundary to the clock where all three
 * planes and the sprite stage have gone idle, across every fixture the
 * corpus has that loads the render heavily. The number decides whether
 * the machine can run at half the clock, where the analyser says every
 * block but the soft CPU closes: the same line is then 2*799 clocks
 * rather than 4*799.
 *
 * Read the numbers as a floor, not a budget. The 6502 is stopped and
 * the PSG silent in every fixture here — neither takes a port A slot
 * that a running machine would — and no fixture puts a heavy sprite
 * load over mode 3's XRAM-palette prologue, which is legal and would
 * be worse than anything measured.
 *
 * The deadline is the beam's, at h == 799: the next line's pixel zero
 * is read then, so the flip must already have landed. Anything at or
 * past it has lost the race, which is what sprite_overrun exists to
 * demonstrate.
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
/* Half clk_sys, rising with it: the PLL's shape, not a divider's. */
static bool rv_phase;

#define PLANE_STATE(n) \
    dut->rootp->rp6502__DOT__gen_mode__BRA__##n##__KET____DOT__vid_mode__DOT__state

/* A scanline is 800 pixels at two clocks; the deadline is the last of
 * them, not the end of the line. */
static const long LINE_CLOCKS = 1600;
static const long LINE_DEADLINE = 2 * 799;

/* The sprite stage walks planes in order and waits only for the plane
 * each sprite lands in (vid_sprite.sv SP_WAIT — it used to wait for all
 * three, and this comment used to say so). Sprites on a light plane
 * overlap a heavy fill for free; the planes+sprites sums below are the
 * coupled case, where the sprites target the plane that was slow. */
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
    /* The terminal renders every line whatever the canvas — vid_term
     * raises run at every line_start — and it neither shares the XRAM
     * nor waits for the planes, so its cost is concurrent, not added.
     * It still has to fit the line on its own. */
    long worst_term;
    /* Where the line's clocks go: each plane's own finish, and the
     * sprite stage's time by state. SP_IDLE=0 SLOT=1 WAIT=2 PLAN=3
     * CLEAR=4 RUN=5. */
    long plane_done[3];
    long sp_state[6];
    long lines;
};

static void clock_cycle()
{
    rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_rv = 0;
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
    b->grants_at_worst = 0;
    b->grants_planes = 0;
    b->grants_sprite = 0;
    b->worst_term = 0;
    for (int i = 0; i < 3; i++)
        b->plane_done[i] = 0;
    for (int i = 0; i < 6; i++)
        b->sp_state[i] = 0;
    b->lines = 0;

    /* Start at a line boundary so the first count is whole. */
    uint16_t prev = dut->rp6502_scanline;
    while (dut->rp6502_scanline == prev)
        clock_cycle();

    for (int line = 0; line < 525; line++)
    {
        prev = dut->rp6502_scanline;
        long clocks = 0, busy_until = 0, planes_until = 0;
        long grants = 0, g_planes = 0, g_sprite = 0, term_until = 0;
        long pdone[3] = {0, 0, 0};
        long spst[6] = {0, 0, 0, 0, 0, 0};
        while (dut->rp6502_scanline == prev)
        {
            clock_cycle();
            clocks++;
            if (!render_idle())
                busy_until = clocks;
            if (PLANE_STATE(0) != 0 || PLANE_STATE(1) != 0
                || PLANE_STATE(2) != 0)
                planes_until = clocks;
            if (PLANE_STATE(0) != 0) pdone[0] = clocks;
            if (PLANE_STATE(1) != 0) pdone[1] = clocks;
            if (PLANE_STATE(2) != 0) pdone[2] = clocks;
            spst[dut->rootp->rp6502__DOT__vid_sprite__DOT__state & 7]++;
            if (dut->rootp->rp6502__DOT__vid_term__DOT__run)
                term_until = clocks;
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
        if (term_until > b->worst_term)
            b->worst_term = term_until;
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
            for (int i = 0; i < 3; i++)
                b->plane_done[i] = pdone[i];
            for (int i = 0; i < 6; i++)
                b->sp_state[i] = spst[i];
        }
    }
}

static void run_case(int *utest_result, const char *name,
                     budget_t *out)
{
    std::string path = std::string(ROMS_DIR "/") + name + ".rp6502";
    FILE *f = fopen(path.c_str(), "rb");
    ASSERT_TRUE(f != NULL);
    budget_t b;
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

    /* One frame is every line: the walk starts on a boundary and takes
     * 525 of them, and the picture is static once tb_quiet says the
     * program has stopped, so which 525 does not matter. */
    measure_frame(&b);

    printf("  %-18s worst %4ld of %ld (%2ld%%) on line %3d"
           "  =  planes %4ld + sprites %4ld"
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
           "slot %3ld wait %4ld plan %3ld clear %4ld run %4ld\n",
           name, b.plane_done[0], b.plane_done[1], b.plane_done[2],
           b.sp_state[1], b.sp_state[2], b.sp_state[3], b.sp_state[4],
           b.sp_state[5]);
    fflush(stdout);

    ASSERT_EQ(b.lines, 525);
    *out = b;
}

/* Fixtures a real program's worth of work should stay inside. The
 * stress fixture is deliberately not one of them — it exists to exceed
 * the machine and prove the counter says so. */
static void expect_in_budget(int *utest_result, const char *name)
{
    budget_t b;
    run_case(utest_result, name, &b);
    ASSERT_LT(b.worst, LINE_DEADLINE);
}

/* Reported, not gated: this one and sprite_overrun are the two that
 * are meant to push past the beam. */
UTEST(budget, sprite_stress_reported)
{
    budget_t b;
    run_case(utest_result, "sprite_stress", &b);
}

UTEST(budget, mode4_cross_plane_640x480) { expect_in_budget(utest_result, "mode4_32"); }
UTEST(budget, mode5_4bpp_256_640x480) { expect_in_budget(utest_result, "mode5_4bpp256"); }
UTEST(budget, mode3_8bpp_640x480) { expect_in_budget(utest_result, "mode3_8bpp"); }
UTEST(budget, mode1_16bpp_8x16) { expect_in_budget(utest_result, "mode1_16bpp8x16"); }
UTEST(budget, mode2_composite) { expect_in_budget(utest_result, "mode2_composite"); }

/* The heavier fixtures the first cut of this test left out. */
UTEST(budget, mode5_32x32) { expect_in_budget(utest_result, "mode5_32x32"); }
UTEST(budget, mode4_sizes) { expect_in_budget(utest_result, "mode4_sizes"); }
UTEST(budget, mode4a_clip) { expect_in_budget(utest_result, "mode4a_clip"); }

/* Built to lose its race, and it does: this is the one fixture that
 * proves the deadline is real and that the counter reports it. */
UTEST(budget, sprite_overrun_loses_on_purpose)
{
    budget_t b;
    run_case(utest_result, "sprite_overrun", &b);
    ASSERT_GE(b.worst, LINE_DEADLINE);
    ASSERT_GT(dut->rootp->rp6502__DOT__vid_sprite__DOT__vid_sprite_overrun,
              0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    oracle_init();
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
