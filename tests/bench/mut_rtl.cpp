/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "mut.h"

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "tb_rom.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vwiring *dut;
static std::vector<uint8_t> mut_rom;
static uint32_t mut_fb[640 * 480];

static std::string mut_tap;

#define FILL_STATE dut->rootp->wiring__DOT__fill__DOT__state
#define SCHED_STATE dut->rootp->wiring__DOT__sched__DOT__state
#define SCHED_PENDING dut->rootp->wiring__DOT__sched__DOT__plane_pending

/* A scanline is 800 pixels of two clocks each. The sprite stage drops any
 * line it has not finished when the pixel counter h is 799, so the deadline
 * is the first clock of the last pixel rather than the end of the line. */
static const long LINE_CLOCKS = 1600;
static const long LINE_DEADLINE = 2 * 799;

struct budget_t
{
    long worst;
    int worst_line;
    long planes_at_worst;
    long sprite_at_worst;
    long worst_planes;
    long grants_at_worst;
    long grants_planes;
    long grants_sprite;
    long worst_term;
    long plane_done[3];
    long sp_state[4];
    long lines;
};

static bool render_idle()
{
    return SCHED_STATE == 0 && FILL_STATE == 0
           && dut->rootp->wiring__DOT__sprite__DOT__state == 0;
}

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

    uint16_t prev = dut->wiring_scanline;
    while (dut->wiring_scanline == prev)
        tb_clock(dut);

    for (int line = 0; line < 525; line++)
    {
        prev = dut->wiring_scanline;
        long clocks = 0, busy_until = 0, planes_until = 0, sprite_until = 0;
        long grants = 0, g_planes = 0, g_sprite = 0, term_until = 0;
        long pdone[3] = {0, 0, 0};
        long spst[4] = {0, 0, 0, 0};
        while (dut->wiring_scanline == prev)
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
            if (dut->rootp->wiring__DOT__sprite__DOT__state != 0)
                sprite_until = clocks;
            spst[dut->rootp->wiring__DOT__sprite__DOT__state & 3]++;
            if (dut->rootp->wiring__DOT__mode0__DOT__run)
                term_until = clocks;
            if (dut->rootp->wiring__DOT__a_any)
            {
                grants++;
                unsigned sel = dut->rootp->wiring__DOT__a_sel;
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

void mut_init(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
}

void mut_free(void)
{
    dut->final();
    delete dut;
    dut = nullptr;
}

void mut_console_start(void)
{
    mut_tap.clear();
}

const char *mut_console(size_t *len)
{
    *len = mut_tap.size();
    return mut_tap.c_str();
}

bool mut_boot(const char *rom)
{
    mut_rom.clear();
    if (!tb_rom_read(rom, mut_rom))
        return false;
    /* The model is rebuilt rather than reset because rst_n does not clear
     * resb. After a reset pulse resb would still be set from the previous
     * boot, and ever_ran would be set on the first clock whether or not the
     * new image loads. */
    dut->final();
    delete dut;
    dut = new Vwiring;

    bool ever_ran = false;
    bool quiet = tb_boot_each(dut, mut_rom, nullptr, [&] {
        if (dut->rootp->wiring__DOT__resb)
            ever_ran = true;
        if (dut->wiring_rv_tx_valid)
            mut_tap.push_back((char)dut->wiring_rv_tx_data);
    });
    if (!quiet)
        fprintf(stderr, "mut_boot: %s never settled\n", rom);
    return ever_ran;
}

void mut_xram(uint32_t addr, uint8_t *dst, size_t len)
{
    auto *r = dut->rootp;
    for (size_t i = 0; i < len; i++)
    {
        uint32_t a = addr + (uint32_t)i;
        size_t wi = a >> 2;
        dst[i] = (a & 3) == 0   ? r->wiring__DOT__xram__DOT__mem0[wi]
                 : (a & 3) == 1 ? r->wiring__DOT__xram__DOT__mem1[wi]
                 : (a & 3) == 2 ? r->wiring__DOT__xram__DOT__mem2[wi]
                                : r->wiring__DOT__xram__DOT__mem3[wi];
    }
}

const uint32_t *mut_frame(int w, int h)
{
    tb_capture(dut, mut_fb, (size_t)w * (size_t)h);
    return mut_fb;
}

mut_budget_t mut_measure(const char *name)
{
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

    if (b.lines != 525)
    {
        fprintf(stderr, "%s: %ld lines in a frame, expected 525\n",
                name, b.lines);
        return MUT_BUDGET_NONE;
    }
    if (b.worst >= LINE_DEADLINE &&
        dut->rootp->wiring__DOT__sprite__DOT__sprite_overrun > 0)
        return MUT_BUDGET_OVER;
    return b.worst < LINE_DEADLINE ? MUT_BUDGET_UNDER : MUT_BUDGET_OVER;
}
