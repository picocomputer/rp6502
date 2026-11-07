/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * psg.sv's savestate port, the bell included. Session A plays and stops at
 * a quiet point; session B has played something else, takes A's image,
 * and then both take one stream of writes and must agree on every sample.
 * B has random words written over everything first, so an item the
 * restore leaves out shows.
 */

#include "Vpsg_sst.h"
#include "Vpsg_sst___024root.h"

#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

#define TICKS 1050

/* Offsets in the savestate's audio region: the structures, the bell's,
 * every slot of the voice arrays, used or not, and the flop words, of
 * which 0x788 is empty. */
static std::vector<int> arrays()
{
    std::vector<int> v;
    for (int a = 0x640; a < 0x680; a++)
        v.push_back(a);
    for (int a = 0x6D0; a < 0x6D8; a++)
        v.push_back(a);
    for (int a = 0x700; a < 0x750; a++)
        v.push_back(a);
    return v;
}
static std::vector<int> flops()
{
    std::vector<int> v;
    for (int a = 0x789; a < 0x78D; a++)
        v.push_back(a);
    return v;
}

/* SKIP_WORD + k for flop word k, SKIP_CFG + b for structure byte b,
 * SKIP_VOICE + k for voice array k. */
enum
{
    SKIP_NONE = -1,
    SKIP_WORD = 0,
    SKIP_CFG = 100,
    SKIP_VOICE = 200
};
static bool skipped(int skip, int a)
{
    if (skip < 0)
        return false;
    if (skip < SKIP_CFG)
        return a == 0x788 + skip;
    if (skip < SKIP_VOICE)
        return ((a >= 0x640 && a < 0x680) || (a >= 0x6D0 && a < 0x6D8))
               && (a & 7) == skip - SKIP_CFG;
    return a >= 0x700 + 16 * (skip - SKIP_VOICE)
           && a < 0x710 + 16 * (skip - SKIP_VOICE);
}

static void edge(Vpsg_sst *d, bool mach)
{
    d->clk_sys = 1;
    d->clk = mach;
    d->eval();
    d->clk_sys = 0;
    d->clk = 0;
    d->eval();
    d->q_we = 0;
    d->xaddr_we = 0;
    d->bel_we = 0;
    d->sst_we = 0;
    d->sst_st_we = 0;
}

static Vpsg_sst *make(const char *name)
{
    Vpsg_sst *d = new Vpsg_sst{name};
    d->clk = 0;
    d->clk_sys = 0;
    d->eval();
    return d;
}

static void snoop(Vpsg_sst *d, uint16_t addr, uint8_t val)
{
    d->q_we = 1;
    d->q_addr = addr;
    d->q_val = val;
    edge(d, true);
}

/* Eight voices gated, voice 3 noise, and a long bell, struck or let go. */
static void program(Vpsg_sst *d, uint16_t at, std::mt19937 &r, bool bell)
{
    d->xaddr_we = 1;
    d->xaddr_wdata = at;
    edge(d, true);
    for (int k = 0; k < 2 * TICKS; k++)
        edge(d, true);
    for (int v = 0; v < 8; v++)
    {
        uint16_t b = (uint16_t)(at + 8 * v);
        uint16_t f = (uint16_t)(600 + (r() % 4000));
        snoop(d, b + 0, (uint8_t)f);
        snoop(d, b + 1, (uint8_t)(f >> 8));
        snoop(d, b + 2, (uint8_t)(0x40 + (r() % 0x80)));
        snoop(d, b + 3, (uint8_t)(0x13 + 16 * (r() % 4)));
        snoop(d, b + 4, (uint8_t)(0x45 + 16 * (r() % 4)));
        snoop(d, b + 5, (uint8_t)(v == 3 ? 0x46 : (r() % 4) << 4 | 6));
        snoop(d, b + 6, (uint8_t)((r() & 0xFE) | 1));
    }
    const uint16_t bf = (uint16_t)(2000 + (r() % 1000));
    const uint8_t bell_bytes[7] = {(uint8_t)bf, (uint8_t)(bf >> 8), 0x80,
                                   0x07, 0x24, 0x30, (uint8_t)(bell ? 1 : 0)};
    for (int i = 0; i < 7; i++)
    {
        d->bel_we = 1;
        d->bel_idx = (uint8_t)i;
        d->bel_val = bell_bytes[i];
        edge(d, true);
    }
}

/* A write stream into the structures: a byte about every few samples and
 * a gate now and then. */
struct Stream
{
    std::mt19937 r;
    uint16_t at;
    bool we = false;
    uint16_t addr = 0;
    uint8_t val = 0;
    Stream(unsigned seed, uint16_t base) : r(seed), at(base) {}
    void next()
    {
        we = (r() % 3000) == 0;
        if (!we)
            return;
        int v = (int)(r() % 8), b = (int)(r() % 7);
        addr = (uint16_t)(at + 8 * v + b);
        val = (uint8_t)r();
        if (b == 5 && v == 3)
            val = (uint8_t)(0x40 | (val & 0x0F));
        if (b == 1)
            val &= 0x1F;
    }
    void apply(Vpsg_sst *d) const
    {
        d->q_we = we;
        d->q_addr = addr;
        d->q_val = val;
    }
    void drive(Vpsg_sst *d)
    {
        next();
        apply(d);
    }
};

static std::vector<uint32_t> image(Vpsg_sst *d, const std::vector<int> &at)
{
    std::vector<uint32_t> img;
    d->sst_own = 1;
    for (int a : at)
    {
        d->sst_addr = (uint16_t)a;
        edge(d, false);
        img.push_back(d->psg_sst_rdata);
    }
    d->sst_own = 0;
    edge(d, false);
    return img;
}

/* As opl's: skip leaves an item out, and cut is cleared in flop word
 * cut_at. */
static void restore(Vpsg_sst *d, const std::vector<uint32_t> &img, int skip,
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

struct Case
{
    unsigned seed = 1;
    int offset = 0;  // the request's clock after a sample's tick
    int lag = 0;     // machine edges from quiet to the stop
    bool remap = false;  // the pointer written again before the request
    int skip = SKIP_NONE;
    int cut_at = -1;     // a flop word B takes with cut cleared
    uint32_t cut = 0;
    long samples = 2000;
};

static long run_case(const Case &c, bool check_image = true)
{
    Vpsg_sst *a = make("a"), *b = make("b");
    std::mt19937 rr(c.seed * 7 + 3), pa(c.seed), pb(c.seed + 99);
    Stream sa(c.seed + 1, 0x8000), sb(c.seed + 1000, 0x9040);

    program(b, 0x9040, pb, false);
    for (long i = 0; i < 55555 + (long)(c.seed % 5) * 3331; i++)
    {
        sb.drive(b);
        edge(b, true);
    }
    program(a, 0x8000, pa, true);
    long pre = 100L * TICKS + (long)(c.seed % 7) * 101;
    for (long i = 0; i < pre; i++)
    {
        sa.drive(a);
        edge(a, true);
        sb.drive(b);
        edge(b, true);
    }
    while (a->rootp->psg__DOT__tickctr != 0)
    {
        sa.drive(a);
        edge(a, true);
    }
    for (int i = 0; i < c.offset; i++)
    {
        sa.drive(a);
        edge(a, true);
    }
    if (c.remap)
    {
        a->xaddr_we = 1;
        a->xaddr_wdata = 0x8000;
        edge(a, true);
    }
    while (!a->psg_quiet)
    {
        sa.drive(a);
        edge(a, true);
    }
    for (int i = 0; i < c.lag; i++)
    {
        sa.drive(a);
        edge(a, true);
    }
    std::vector<int> at = arrays(), fl = flops();
    at.insert(at.end(), fl.begin(), fl.end());
    std::vector<uint32_t> img = image(a, at);

    while (!b->psg_quiet)
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

    Stream s(c.seed + 555, 0x8000);
    long n = 0, bad = -1;
    for (long i = 0; i < (c.samples + 2) * TICKS && n < c.samples; i++)
    {
        s.next();
        s.apply(a);
        s.apply(b);
        edge(a, true);
        edge(b, true);
        if (a->psg_tick != b->psg_tick
            || (a->psg_tick
                && (a->psg_l != b->psg_l || a->psg_r != b->psg_r)))
        {
            bad = n;
            break;
        }
        n += a->psg_tick;
    }
    a->final();
    b->final();
    delete a;
    delete b;
    return bad;
}

UTEST(psg_sst, a_save_mid_note_comes_back)
{
    Case c;
    c.seed = 11;
    c.offset = 300;
    ASSERT_EQ(-1L, run_case(c));
}

/* 64 request points across one walk and the sample around it, each
 * stopped at quiet plus 0, 4 or 23 clocks. */
UTEST(psg_sst, every_save_point_in_a_sample_comes_back)
{
    static const int lags[] = {0, 4, 23};
    for (int i = 0; i < 64; i++)
    {
        Case c;
        c.seed = 100 + (unsigned)i;
        c.offset = i * 16;
        c.lag = lags[i % 3];
        c.samples = 1000;
        long bad = run_case(c);
        if (bad != -1)
            fprintf(stderr, "  offset %d lag %d: differs at %ld\n", c.offset,
                    c.lag, bad);
        ASSERT_EQ(-1L, bad);
    }
}

/* A save between a pointer write and the walk that clears the envelopes
 * and the noise comes back, and the pending clear, cleared in B, changes
 * the sound. */
UTEST(psg_sst, a_save_after_a_pointer_write_comes_back)
{
    Case c;
    c.seed = 41;
    c.offset = 300;
    c.remap = true;
    ASSERT_EQ(-1L, run_case(c));
    c.cut_at = 0x789;
    c.cut = 1u << 25;
    long bad = run_case(c, false);
    fprintf(stderr, "  the clear cleared: differs at %ld\n", bad);
    ASSERT_GE(bad, 0L);
}

/* Each flop word, each structure byte and each voice array, left out,
 * changes the sound. Word 1 holds only clr, which
 * a_save_after_a_pointer_write_comes_back cuts. */
UTEST(psg_sst, every_saved_item_matters)
{
    std::vector<int> items;
    for (int k = 2; k < 5; k++)
        items.push_back(SKIP_WORD + k);
    for (int b = 0; b < 7; b++)
        items.push_back(SKIP_CFG + b);
    for (int k = 0; k < 5; k++)
        items.push_back(SKIP_VOICE + k);
    for (int skip : items)
    {
        Case c;
        c.seed = 23;
        c.offset = 200;
        c.skip = skip;
        c.samples = 600;
        long bad = run_case(c, false);
        fprintf(stderr, "  without %s %d: differs at %ld\n",
                skip < SKIP_CFG ? "word" : skip < SKIP_VOICE ? "byte" : "array",
                skip % 100, bad);
        ASSERT_NE(-1L, bad);
    }
}

UTEST_MAIN();
