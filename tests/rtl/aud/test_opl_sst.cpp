/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * opl.sv's savestate port. Session A plays and stops at a quiet point;
 * session B has played something else, takes A's image, and then both take
 * one stream of writes and must agree on every sample and on the drop
 * count. B has random words written over everything first, so an item the
 * restore leaves out shows.
 */

#include "Vopl_sst.h"
#include "Vopl_sst___024root.h"

#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#define TICKS 1014

/* Offsets in the savestate's audio region. */
static std::vector<int> arrays()
{
    std::vector<int> v;
    for (int a = 0x400; a < 0x600; a++)
        v.push_back(a);
    for (int a = 0x680; a < 0x6A0; a++)
        v.push_back(a);
    for (int a = 0x6C0; a < 0x6D0; a++)
        v.push_back(a);
    return v;
}
static std::vector<int> flops()
{
    std::vector<int> v;
    for (int a = 0x780; a < 0x788; a++)
        v.push_back(a);
    return v;
}

/* skip leaves an item out of B's restore: SKIP_WORD + k for flop word k,
 * SKIP_ARRAY + n for the queue, registers, slots or channels. */
enum
{
    SKIP_NONE = -1,
    SKIP_WORD = 0,
    SKIP_ARRAY = 100
};
static bool skipped(int skip, int a)
{
    if (skip < 0)
        return false;
    if (skip < SKIP_ARRAY)
        return a == 0x780 + skip;
    switch (skip - SKIP_ARRAY)
    {
    case 0: return a >= 0x400 && a < 0x500;
    case 1: return a >= 0x500 && a < 0x600;
    case 2: return a >= 0x680 && a < 0x6A0;
    default: return a >= 0x6C0 && a < 0x6D0;
    }
}

static void edge(Vopl_sst *d, bool mach)
{
    d->clk_sys = 1;
    d->clk = mach;
    d->eval();
    d->clk_sys = 0;
    d->clk = 0;
    d->eval();
    d->q_we = 0;
    d->xaddr_we = 0;
    d->drops_take = 0;
    d->sst_we = 0;
    d->sst_st_we = 0;
}

static Vopl_sst *make(const char *name)
{
    Vopl_sst *d = new Vopl_sst{name};
    d->clk = 0;
    d->clk_sys = 0;
    d->eval();
    return d;
}

/* Any register but channel 0's, which keeps its note and its feedback
 * sounding through every save. */
static int legal(std::mt19937 &r)
{
    for (;;)
    {
        int a = (int)(r() & 0xFF);
        int hi = a >> 4, lo = a & 15, g = a >> 5, o = a & 31;
        if (a == 0x01 || a == 0x08 || a == 0xBD)
            return a;
        if ((hi == 0xA || hi == 0xB || hi == 0xC) && (lo <= 11 || lo >= 14)
            && lo != 0 && lo != 9 && lo != 14)
            return a;
        if ((g == 1 || g == 2 || g == 3 || g == 4 || g == 7) && o < 22
            && (o & 7) < 6 && o != 0 && o != 3)
            return a;
    }
}

static int value(std::mt19937 &r, int a)
{
    int v = (int)(r() & 0xFF);
    if (a == 0x01)
        /* WSE and test bit 3 often, test bit 7 now and then. */
        return (v & 0x28) | ((r() % 8) == 0 ? 0x80 : 0);
    if (a == 0x08)
        return v & 0xC0;
    if (a == 0xBD)
        return v | 0x20;
    return v;
}

/* A write stream on a page: a random write about every other sample, a
 * 300-write burst when asked, and nothing from the soft CPU. */
struct Stream
{
    std::mt19937 r;
    int page;
    int burst = 0;
    bool we = false;
    uint16_t addr = 0;
    uint8_t val = 0;
    Stream(unsigned seed, int pg) : r(seed), page(pg) {}
    void next()
    {
        we = burst > 0 || (r() % 2000) == 0;
        if (we)
        {
            int a = legal(r);
            addr = (uint16_t)(page << 8 | a);
            val = (uint8_t)value(r, a);
            if (burst > 0)
                burst--;
        }
    }
    void apply(Vopl_sst *d) const
    {
        d->q_we = we;
        d->q_addr = addr;
        d->q_val = val;
    }
    void drive(Vopl_sst *d)
    {
        next();
        apply(d);
    }
};

static void put(Vopl_sst *d, int page, int reg, int val)
{
    d->q_we = 1;
    d->q_addr = (uint16_t)(page << 8 | reg);
    d->q_val = (uint8_t)val;
    edge(d, true);
}

static void map(Vopl_sst *d, int page)
{
    d->xaddr_we = 1;
    d->xaddr_wdata = (uint16_t)(page << 8);
    edge(d, true);
}

/* A melodic channel with feedback, vibrato and tremolo depth, and the
 * rhythm section, all through the queue. */
static void program(Vopl_sst *d, int page)
{
    static const int prog[][2] = {
        {0x01, 0x20}, {0x20, 0xE1}, {0x23, 0xC1}, {0x40, 0x10},
        {0x43, 0x00}, {0x60, 0xF4}, {0x63, 0xF4}, {0x80, 0x57},
        {0x83, 0x57}, {0xE0, 0x00}, {0xE3, 0x02}, {0xC0, 0x0E},
        {0xA0, 0x98}, {0xB0, 0x31}, {0xA6, 0x50}, {0xB6, 0x09},
        {0xA7, 0x60}, {0xB7, 0x09}, {0xA8, 0x70}, {0xB8, 0x09},
        {0x30, 0x01}, {0x31, 0x02}, {0x32, 0x01}, {0x33, 0x01},
        {0x34, 0x01}, {0x35, 0x01}, {0x50, 0x00}, {0x51, 0x00},
        {0x52, 0x00}, {0x53, 0x00}, {0x54, 0x00}, {0x55, 0x00},
        {0x70, 0xF6}, {0x71, 0xF6}, {0x72, 0xF6}, {0x73, 0xF6},
        {0x74, 0xF6}, {0x75, 0xF6}, {0x90, 0x46}, {0x91, 0x46},
        {0x92, 0x46}, {0x93, 0x46}, {0x94, 0x46}, {0x95, 0x46},
        {0xC6, 0x0B}, {0xBD, 0xFF},
    };
    for (auto &w : prog)
        put(d, page, w[0], w[1]);
}

static std::vector<uint32_t> image(Vopl_sst *d, const std::vector<int> &at)
{
    std::vector<uint32_t> img;
    d->sst_own = 1;
    for (int a : at)
    {
        d->sst_addr = (uint16_t)a;
        edge(d, false);
        img.push_back(d->opl_sst_rdata);
    }
    d->sst_own = 0;
    edge(d, false);
    return img;
}

/* B, stopped at a quiet point, takes img: random words everywhere, then
 * the image but for skip, and with cut cleared in flop word cut_at. The
 * flop words go in clocked and parked. */
static void restore(Vopl_sst *d, const std::vector<uint32_t> &img, int skip,
                    int cut_at, uint32_t cut, std::mt19937 &r)
{
    std::vector<int> arr = arrays(), fl = flops();
    d->sst_own = 1;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < arr.size(); i++)
        {
            if (pass && skipped(skip, arr[i]))
                continue;
            d->sst_addr = (uint16_t)arr[i];
            d->sst_wdata = pass ? img[i] : (uint32_t)r();
            d->sst_we = 1;
            edge(d, false);
        }
    d->sst_own = 0;
    edge(d, false);
    d->sst_park = 1;
    for (int k = 0; k < 5; k++)
        edge(d, true);
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 0; i < fl.size(); i++)
        {
            if (pass && skipped(skip, fl[i]))
                continue;
            d->sst_addr = (uint16_t)fl[i];
            d->sst_wdata = pass ? img[arr.size() + i]
                                      & ~(fl[i] == cut_at ? cut : 0u)
                                : (uint32_t)r();
            d->sst_st_we = 1;
            edge(d, true);
        }
    for (int k = 0; k < 50; k++)
        edge(d, true);
}

/* What the last stop held, for the sweep's report. */
static unsigned g_q_len, g_rst_pend;

/* Flop states a stop seldom finds: the tremolo's step taken with test bit
 * 3 clear, the tremolo falling, and the EG timer's carry. */
enum
{
    RARE_NONE,
    RARE_TSTEP,
    RARE_TDIR,
    RARE_EGC
};
static uint32_t peek(Vopl_sst *d, int a)
{
    return image(d, std::vector<int>{a})[0];
}
static bool rare_held(Vopl_sst *d, int rare)
{
    auto *r = d->rootp;
    switch (rare)
    {
    case RARE_TSTEP: return r->opl__DOT__tstep && !r->opl__DOT__test3;
    case RARE_TDIR: return r->opl__DOT__tdir;
    case RARE_EGC: return r->opl__DOT__egc;
    default: return true;
    }
}

struct Case
{
    unsigned seed = 1;
    bool clear = false;  // a pointer write starts the frame the save is in
    int offset = 0;      // the request's clock after the frame's start
    int lag = 0;         // machine edges from quiet to the stop
    int rare = RARE_NONE;  // frames run on until the stop holds it
    int skip = SKIP_NONE;
    int cut_at = -1;     // a flop word B takes with cut cleared
    uint32_t cut = 0;
    long samples = 2000;
};

/* The first sample A and B disagree on, or -1. */
static long run_case(const Case &c, bool check_image = true)
{
    Vopl_sst *a = make("a"), *b = make("b");
    std::mt19937 rr(c.seed * 7 + 3);
    Stream sa(c.seed, 0x12), sb(c.seed + 1000, 0x34);

    /* B starts earlier on its own page and its own stream. */
    map(b, 0x34);
    program(b, 0x34);
    for (long i = 0; i < 77777 + (long)(c.seed % 5) * 3331; i++)
    {
        sb.drive(b);
        edge(b, true);
    }

    map(a, 0x12);
    program(a, 0x12);
    long pre = 150L * TICKS + (long)(c.seed % 7) * 101;
    for (long i = 0; i < pre; i++)
    {
        sa.drive(a);
        edge(a, true);
        sb.drive(b);
        edge(b, true);
    }
    /* The request's frame. In clear mode a pointer write between frames
     * makes the next one the reset, and a burst fills the queue either
     * way. */
    while (a->rootp->opl__DOT__tickctr != (c.clear ? 300 : 0))
    {
        sa.drive(a);
        edge(a, true);
    }
    if (c.clear)
        map(a, 0x12);
    sa.burst = 300;
    for (int i = 0; i < c.offset; i++)
    {
        sa.drive(a);
        edge(a, true);
    }
    while (!a->opl_quiet)
    {
        sa.drive(a);
        edge(a, true);
    }
    for (int i = 0; i < c.lag; i++)
    {
        sa.drive(a);
        edge(a, true);
    }
    if (c.rare == RARE_EGC)
    {
        /* The timer at its top with sub set, which it reaches on its own
         * once in 2^19 frames, carries on the next. */
        a->sst_addr = 0x784;
        a->sst_wdata = peek(a, 0x784) | 0x3FFFFu | 1u << 19;
        a->sst_st_we = 1;
        edge(a, true);
    }
    for (long f = 0; c.rare != RARE_NONE && !rare_held(a, c.rare); f++)
    {
        if (f == 20000)
        {
            delete a;
            delete b;
            return -3;
        }
        do
        {
            sa.drive(a);
            edge(a, true);
        } while (a->opl_quiet);
        while (!a->opl_quiet)
        {
            sa.drive(a);
            edge(a, true);
        }
    }
    std::vector<int> at = arrays(), fl = flops();
    at.insert(at.end(), fl.begin(), fl.end());
    std::vector<uint32_t> img = image(a, at);
    g_q_len = a->rootp->opl__DOT__q_len;
    g_rst_pend = a->rootp->opl__DOT__rst_pend;

    while (!b->opl_quiet)
    {
        sb.drive(b);
        edge(b, true);
    }
    restore(b, img, c.skip, c.cut_at, c.cut, rr);
    if (check_image && c.skip == SKIP_NONE && !c.cut)
    {
        std::vector<uint32_t> back = image(b, at);
        for (size_t i = 0; i < at.size(); i++)
            if (back[i] != img[i])
            {
                fprintf(stderr, "  image %03X: %08X, A had %08X\n", at[i],
                        back[i], img[i]);
                delete a;
                delete b;
                return -2;
            }
    }
    b->sst_park = 0;

    Stream s(c.seed + 555, 0x12);
    s.burst = 40;
    long n = 0, bad = -1;
    for (long i = 0; i < (c.samples + 2) * TICKS && n < c.samples; i++)
    {
        s.next();
        s.apply(a);
        s.apply(b);
        if (i == 900L * TICKS)
            a->drops_take = b->drops_take = 1;
        edge(a, true);
        edge(b, true);
        if (a->opl_valid != b->opl_valid || a->opl_drops != b->opl_drops
            || (a->opl_valid && a->opl_out != b->opl_out))
        {
            bad = n;
            break;
        }
        n += a->opl_valid;
    }
    a->final();
    b->final();
    delete a;
    delete b;
    return bad;
}

UTEST(opl_sst, a_save_with_the_queue_full_comes_back)
{
    Case c;
    c.seed = 11;
    c.offset = 400;
    ASSERT_EQ(-1L, run_case(c));
}

UTEST(opl_sst, a_save_across_the_reset_comes_back)
{
    Case c;
    c.seed = 12;
    c.clear = true;
    c.offset = 3;
    ASSERT_EQ(-1L, run_case(c));
}

/* 64 request points across one frame, the reset's half the time, each
 * stopped at quiet plus 0, 4 or 23 clocks. */
UTEST(opl_sst, every_save_point_in_a_frame_comes_back)
{
    static const int lags[] = {0, 4, 23};
    int full = 0, queued = 0, reset = 0;
    for (int i = 0; i < 64; i++)
    {
        Case c;
        c.seed = 100 + (unsigned)i;
        c.clear = i & 1;
        c.offset = i * 16;
        c.lag = lags[i % 3];
        c.samples = 1000;
        long bad = run_case(c);
        if (bad != -1)
            fprintf(stderr, "  offset %d lag %d clear %d: differs at %ld\n",
                    c.offset, c.lag, (int)c.clear, bad);
        ASSERT_EQ(-1L, bad);
        full += g_q_len == 256;
        queued += g_q_len != 0;
        reset += g_rst_pend != 0;
    }
    fprintf(stderr, "  64 stops: %d with the queue full, %d with writes queued, "
                    "%d with the reset pending\n", full, queued, reset);
}

/* Stops on each rare flop state come back, and the state, cleared in B,
 * changes the sound. */
UTEST(opl_sst, a_save_on_a_rare_step_comes_back)
{
    static const struct
    {
        int rare, word;
        uint32_t bit;
    } rares[] = {
        {RARE_TSTEP, 0x782, 1u << 29},
        {RARE_TDIR, 0x782, 1u << 31},
        {RARE_EGC, 0x784, 1u << 18},
    };
    for (auto &k : rares)
    {
        Case c;
        c.seed = 30 + (unsigned)k.rare;
        c.offset = 100;
        c.rare = k.rare;
        ASSERT_EQ(-1L, run_case(c));
        c.cut_at = k.word;
        c.cut = k.bit;
        long bad = run_case(c, false);
        fprintf(stderr, "  rare %d, cleared: differs at %ld\n", k.rare, bad);
        ASSERT_GE(bad, 0L);
    }
}

/* Each flop word and each array, left out, changes the sound or the drop
 * count. */
UTEST(opl_sst, every_saved_item_matters)
{
    for (int skip = 0; skip < SKIP_ARRAY + 4; skip++)
    {
        if (skip == 8)
            skip = SKIP_ARRAY;
        Case c;
        c.seed = 21;
        c.offset = 200;
        c.skip = skip;
        c.samples = 600;
        long bad = run_case(c, false);
        fprintf(stderr, "  without %s %d: differs at %ld\n",
                skip < SKIP_ARRAY ? "word" : "array",
                skip < SKIP_ARRAY ? skip : skip - SKIP_ARRAY, bad);
        ASSERT_NE(-1L, bad);
    }
}

UTEST_MAIN();
