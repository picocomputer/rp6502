/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * pcm.sv's savestate port, the OPL2's resampler and PCM, as wiring.sv has
 * it. Session A plays and stops at a quiet point; session B has played
 * something else, takes A's image, and then both take one stream of input
 * and writes and must agree on every clock. Both clocks run while the
 * image is taken and put back, with no tick and no input, as wiring.sv's
 * hold has them. B has random words written over its memories first, so
 * an item the restore leaves out shows.
 */

#include "Vtb_aud_sst.h"
#include "Vtb_aud_sst___024root.h"

#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <random>
#include <vector>

/* The OPL's rate in and the tick out, as wiring.sv clocks them. */
#define OPL_TICKS 1014
#define TICKS 1050
#define LEAD 24

static std::vector<int> arrays()
{
    std::vector<int> v;
    for (int a = 0x000; a < 0x400; a++)
        v.push_back(a);
    for (int a = 0x600; a < 0x640; a++)
        v.push_back(a);
    for (int a = 0x7C0; a < 0x800; a++)
        v.push_back(a);
    return v;
}
static std::vector<int> flops()
{
    std::vector<int> v;
    for (int a = 0x790; a < 0x798; a++)
        v.push_back(a);
    for (int a = 0x798; a < 0x79B; a++)
        v.push_back(a);
    return v;
}

/* SKIP_WORD + n for flop word 0x790 + n, SKIP_ARRAY + n for the FIFO,
 * pcm's history or rsmp's. */
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
        return a == 0x790 + skip;
    switch (skip - SKIP_ARRAY)
    {
    case 0: return a < 0x400;
    case 1: return a >= 0x600 && a < 0x640;
    default: return a >= 0x7C0 && a < 0x800;
    }
}

static void edge(Vtb_aud_sst *d)
{
    d->clk = 1;
    d->eval();
    d->clk = 0;
    d->eval();
    d->in_valid = 0;
    d->step = 0;
    d->tick = 0;
    d->ctrl_we = 0;
    d->step_we = 0;
    d->data_we = 0;
    d->byte_we = 0;
    d->clear_we = 0;
    d->commit_we = 0;
    d->sst_we = 0;
}

static Vtb_aud_sst *make(const char *name)
{
    Vtb_aud_sst *d = new Vtb_aud_sst{name};
    d->clk = 0;
    d->eval();
    return d;
}

/* The two rates and the PCM writes, committed now and then, so a save
 * often finds bytes stored and not yet committed. The rates' counters
 * belong to the machine, so they carry on from A's through the restore. */
struct Stream
{
    std::mt19937 r;
    long opl = 0, psg = 0;
    bool in_valid = false, tick = false, data = false, byte = false;
    bool clear = false, open = false, commit = false;
    unsigned staged = 0, staged_was = 0;
    int16_t sample = 0;
    uint32_t word = 0;
    Stream(unsigned seed) : r(seed) {}
    void next(unsigned level)
    {
        in_valid = ++opl == OPL_TICKS;
        if (in_valid)
            opl = 0;
        tick = ++psg == TICKS;
        if (tick)
            psg = 0;
        sample = (int16_t)r();
        word = (uint32_t)r();
        data = level + staged <= 4096 - 4 && (r() % 300) == 0;
        byte = !data && level + staged < 4096 && (r() % 3000) == 0;
        clear = (r() % 4000000) == 0;
        commit = !data && !byte && (r() % 1500) == 0;
        open = false;
        staged_was = staged;
        staged = commit ? 0 : staged + (data ? 4 : byte ? 1 : 0);
    }
    void apply(Vtb_aud_sst *d) const
    {
        d->in_valid = in_valid;
        d->in_sample = (uint16_t)sample;
        d->step = tick;
        d->tick = tick;
        d->data_we = data;
        d->byte_we = byte;
        d->clear_we = clear;
        d->commit_we = commit;
        if (data || byte)
            d->wdata = word;
    }
    /* Clear of both rates for the lead. */
    bool calm() const
    {
        return opl < OPL_TICKS - 1 - LEAD && psg < TICKS - 1 - LEAD;
    }
};

static void open_pcm(Vtb_aud_sst *d, unsigned rate, unsigned ctrl)
{
    d->step_we = 1;
    d->wdata = (uint32_t)(((uint64_t)rate << 32) / 48000 - 1);
    edge(d);
    d->ctrl_we = 1;
    d->wdata = ctrl;
    edge(d);
}

static void run(Vtb_aud_sst *d, Stream &s, long n, bool halted = false)
{
    for (long i = 0; i < n; i++)
    {
        s.next(d->pcm_level);
        if (halted)
        {
            s.data = s.byte = s.clear = s.commit = false;
            s.staged = s.staged_was;
        }
        s.apply(d);
        edge(d);
    }
}

static std::vector<uint32_t> image(Vtb_aud_sst *d, const std::vector<int> &at)
{
    std::vector<uint32_t> img;
    d->sst_own = 1;
    for (int a : at)
    {
        d->sst_addr = (uint16_t)a;
        edge(d);
        img.push_back(d->rsmp_sst_rdata | d->pcm_sst_rdata);
    }
    d->sst_own = 0;
    edge(d);
    return img;
}

/* skip leaves an item out, and cut is cleared in flop word cut_at. */
static void restore(Vtb_aud_sst *d, const std::vector<uint32_t> &img, int skip,
                    int cut_at, uint32_t cut, std::mt19937 &r)
{
    std::vector<int> arr = arrays(), fl = flops();
    d->sst_own = 1;
    /* One pass in address order, as the firmware's AUD row writes them.
     * A word left out takes a random word, so B does not keep a value
     * that happens to be A's. */
    for (size_t i = 0; i < arr.size() + fl.size(); i++)
    {
        int at = i < arr.size() ? arr[i] : fl[i - arr.size()];
        d->sst_addr = (uint16_t)at;
        d->wdata = skipped(skip, at)
            ? (uint32_t)r()
            : img[i] & ~(at == cut_at ? cut : 0u);
        d->sst_we = 1;
        edge(d);
    }
    d->sst_own = 0;
    for (int k = 0; k < 50; k++)
        edge(d);
}

struct Case
{
    unsigned seed = 1;
    int offset = 0;    // the request's clock after a tick
    int lag = 0;       // machine edges from quiet to the stop
    bool reopen = false;  // an open just before the request
    int warm = 0;         // ticks from that open to the request
    int skip = SKIP_NONE;
    int cut_at = -1;      // a flop word B takes with cut cleared
    uint32_t cut = 0;
    long samples = 2000;
    uint32_t under = 0;   // A's underrun count, set before the request
    bool close = false;   // A's PCM: closed just before the request
    bool reopen_after = false; // both opened again after the restore
    bool starve = false;  // A's FIFO cleared before the request and no
                          // bytes after it, so it plays dry
};

static unsigned g_rst_pend, g_level;

static long run_case(const Case &c, bool check_image = true)
{
    Vtb_aud_sst *a = make("a"), *b = make("b");
    std::mt19937 rr(c.seed * 7 + 3);
    Stream sa(c.seed), sb(c.seed + 1000);
    sb.psg = 333;
    sb.opl = 77;

    open_pcm(b, 11025, 1);
    run(b, sb, 66666 + (long)(c.seed % 5) * 3331);
    open_pcm(a, 22050, 7);
    run(a, sa, 30000);
    for (long i = 0; i < 200L * TICKS + (long)(c.seed % 7) * 101; i++)
    {
        sa.next(a->pcm_level);
        sa.apply(a);
        edge(a);
        sb.next(b->pcm_level);
        sb.apply(b);
        edge(b);
    }
    while (sa.psg != 0)
        run(a, sa, 1);
    run(a, sa, c.offset);
    if (c.under)
    {
        a->sst_addr = 0x795;
        a->wdata = c.under;
        a->sst_we = 1;
        edge(a);
    }
    if (c.starve)
    {
        a->clear_we = 1;
        edge(a);
    }
    if (c.close)
    {
        a->ctrl_we = 1;
        a->wdata = 0;
        edge(a);
    }
    if (c.reopen)
    {
        a->ctrl_we = 1;
        a->wdata = 7;
        edge(a);
        run(a, sa, (long)c.warm * TICKS);
    }
    /* From the request to the stop the soft CPU is halted. */
    while (!(a->rsmp_quiet && a->pcm_quiet && sa.calm()))
        run(a, sa, 1, true);
    run(a, sa, c.lag, true);
    std::vector<int> at = arrays(), fl = flops();
    at.insert(at.end(), fl.begin(), fl.end());
    std::vector<uint32_t> img = image(a, at);
    g_rst_pend = a->rootp->tb_aud__DOT__pcm__DOT__rst_pend;
    g_level = a->pcm_level;

    while (!(b->rsmp_quiet && b->pcm_quiet && sb.calm()))
        run(b, sb, 1, true);
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

    if (c.reopen_after)
        for (Vtb_aud_sst *d : {a, b})
        {
            d->ctrl_we = 1;
            d->wdata = 7;
            edge(d);
        }
    Stream s = sa;
    s.r.seed(c.seed + 555);
    long n = 0, bad = -1;
    for (long i = 0; i < (c.samples + 2) * TICKS && n < c.samples; i++)
    {
        s.next(a->pcm_level);
        if (c.starve || c.close)
        {
            s.data = s.byte = s.commit = false;
            s.staged = s.staged_was;
        }
        s.apply(a);
        s.apply(b);
        edge(a);
        edge(b);
        if (a->rsmp_out != b->rsmp_out || a->rsmp_valid != b->rsmp_valid
            || a->pcm_l != b->pcm_l || a->pcm_r != b->pcm_r
            || a->pcm_level != b->pcm_level || a->pcm_under != b->pcm_under)
        {
            bad = n;
            break;
        }
        n += s.tick;
    }
    a->final();
    b->final();
    delete a;
    delete b;
    return bad;
}

UTEST(aud_sst, a_save_mid_stream_comes_back)
{
    Case c;
    c.seed = 11;
    c.offset = 300;
    c.under = 0x5A5A5A5;
    ASSERT_EQ(-1L, run_case(c));
    ASSERT_GT(g_level, 2048u);
}

UTEST(aud_sst, a_save_just_after_an_open_comes_back)
{
    Case c;
    c.seed = 12;
    c.offset = 400;
    c.reopen = true;
    ASSERT_EQ(-1L, run_case(c));
    ASSERT_EQ(1u, g_rst_pend);
}

/* A save just after a close, with the player still playing out what its
 * filter holds, comes back. */
UTEST(aud_sst, a_save_while_a_close_plays_out_comes_back)
{
    Case c;
    c.seed = 51;
    c.offset = 400;
    c.close = true;
    c.samples = 3000;
    ASSERT_EQ(-1L, run_case(c));
    ASSERT_EQ(0u, g_level);
}

/* 64 request points across one sample, each stopped at quiet plus 0, 4 or
 * 23 clocks. */
UTEST(aud_sst, every_save_point_in_a_sample_comes_back)
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

/* Each flop word and each array, left out, changes the sound, the level
 * or the underrun count: the write pointer by where the bytes stored and
 * not yet committed land. */
UTEST(aud_sst, every_saved_item_matters)
{
    std::vector<int> items;
    for (int k = 0; k < 8; k++)
        items.push_back(SKIP_WORD + k);
    for (int k = 8; k < 11; k++)
        items.push_back(SKIP_WORD + k);
    for (int k = 0; k < 3; k++)
        items.push_back(SKIP_ARRAY + k);
    for (int skip : items)
    {
        Case c;
        c.seed = 21;
        c.offset = 200;
        c.skip = skip;
        c.samples = 600;
        c.under = 3;
        /* The newest samples matter only to the fade of a dry take. */
        c.starve = skip == SKIP_WORD + 7;
        /* The step written matters only to the next open. */
        c.reopen_after = skip == SKIP_WORD + 10;
        long bad = run_case(c, false);
        fprintf(stderr, "  without %s %03X: differs at %ld\n",
                skip < SKIP_ARRAY ? "word" : "array",
                skip < SKIP_ARRAY ? 0x790 + skip : skip - SKIP_ARRAY, bad);
        ASSERT_NE(-1L, bad);
    }
}

UTEST_MAIN();
