/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * pcm.sv against core/aud/pcm.c at 48 kHz, in lockstep: every write,
 * clear, open and close goes to both on the same clock, the level is
 * compared on every clock, the tick's clock included, and every sample
 * is compared, both before its rounding and after. An event on the
 * tick's clock is after that sample.
 */

#include "Vtb_aud.h"
#include "Vtb_aud___024root.h"
#include "verilated.h"

#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <vector>

extern "C"
{
#include "core/aud/rsmp.h"
#include "pcm_shim.h"
}

UTEST_MAIN();

/* 50.4 MHz / 48 kHz, psg_tick's period. */
#define F 1050
#define BUF 4096
/* pcm.sv's state_t. */
#define S_IDLE 0
#define S_SUM 5

static Vtb_aud *dut;

static uint32_t rs = 1;
static uint32_t rnd()
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

struct fmt_t
{
    uint32_t rate;
    uint8_t bits;
    uint8_t channels;
};

enum
{
    OP_STEP,
    OP_OPEN,
    OP_CLOSE,
    OP_DATA,
    OP_BYTE,
    OP_CLEAR,
    OP_COMMIT
};

struct op_t
{
    long at;
    int kind;
    uint32_t v;
    fmt_t fmt;
};

/* Full-scale samples: Nyquist squares with phase flips, which reach the
 * filter's peak, and either rail held or at random. pos counts bytes from
 * the last open or clear, which is where pcm.c's frames start. */
struct rails_t
{
    uint32_t pos, bps, frame;
    int32_t val;
    int seg, kind, flip;
    uint32_t k;
};

static char why[512];

struct sim_t
{
    long clock = 0;
    bool open = false;
    int32_t want_l = 0, want_r = 0;
    std::deque<op_t> q;
    /* Clock of the last op queued, so the bus carries one a clock. */
    long last = -1;
    long samples = 0, taken_ops = 0;
    /* Ops that landed on the tick's clock and in the player's 110, by
     * kind. */
    long on_tick[7] = {}, in_work[7] = {};
    /* Bytes stored since the last commit, which pcm.c gets at it. */
    std::vector<uint8_t> staged;
    /* Takes of a whole frame and of silence, ticks with the FIFO full,
     * and underruns counted. */
    long whole = 0, dry = 0, brim = 0, under = 0;
    /* This clock's underrun, and whether it opened the device. */
    uint32_t inc = 0;
    bool opened = false;
    fmt_t fmt = {};
    uint8_t align[4] = {};
    /* The last sample's unrounded blend, both channels; the most
     * compared, and the largest output. */
    int64_t want_v[2] = {};
    long v_checked = 0;
    int32_t peak = 0;
    bool rails = false;
    rails_t rl = {};

    /* rsmp beside it, on the other ROM port. */
    bool rsmp = false;
    rsmp_t r;
    uint64_t r_step = 0;
    long r_next = 0;
    int r_i = 0;
    std::vector<int16_t> rc, rv;
};

static void push(sim_t &s, long at, int kind, uint32_t v = 0, fmt_t fmt = {})
{
    if (at <= s.last)
        at = s.last + 1;
    s.last = at;
    s.q.push_back({at, kind, v, fmt});
    if (kind == OP_OPEN)
    {
        s.rl.bps = fmt.bits / 8u;
        s.rl.frame = s.rl.bps * fmt.channels;
    }
    if (kind == OP_OPEN || kind == OP_CLEAR)
        s.rl.pos = 0;
}

static void fresh()
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vtb_aud;
    dut->clk = 0;
    dut->eval();
}

static int16_t r_gen(int n) { return (int16_t)(30000.0 * __builtin_sin(0.37 * n)); }

/* Puts op on the inputs for this clock and gives it to pcm.c. */
static void apply(sim_t &s, const op_t &o)
{
    switch (o.kind)
    {
    case OP_STEP:
        dut->step_we = 1;
        dut->wdata = (uint32_t)(shim_pcm_step(o.fmt.rate) - 1);
        break;
    case OP_OPEN:
        dut->ctrl_we = 1;
        dut->wdata = 1u | (o.fmt.bits == 16 ? 2u : 0u) | (o.fmt.channels == 2 ? 4u : 0u);
        shim_pcm_reset(o.fmt.rate, o.fmt.bits, o.fmt.channels);
        s.staged.clear();
        s.opened = true;
        s.open = true;
        s.fmt = o.fmt;
        break;
    case OP_CLOSE:
        dut->ctrl_we = 1;
        dut->wdata = 0;
        s.open = false;
        break;
    case OP_DATA:
    {
        const uint8_t b[4] = {(uint8_t)o.v, (uint8_t)(o.v >> 8), (uint8_t)(o.v >> 16),
                              (uint8_t)(o.v >> 24)};
        s.align[dut->rootp->tb_aud__DOT__pcm__DOT__stage & 3]++;
        if (dut->rootp->tb_aud__DOT__pcm__DOT__stage & 3)
        {
            /* A word lands aligned: this one goes as its bytes. */
            for (int k = 3; k >= 1; k--)
                s.q.push_front({s.clock, OP_BYTE, b[k], {}});
            dut->byte_we = 1;
            dut->wdata = b[0] * 0x01010101u;
            s.staged.push_back(b[0]);
            break;
        }
        dut->data_we = 1;
        dut->wdata = o.v;
        s.staged.insert(s.staged.end(), b, b + 4);
        break;
    }
    case OP_BYTE:
    {
        dut->byte_we = 1;
        dut->wdata = (o.v & 0xFF) * 0x01010101u;
        s.staged.push_back((uint8_t)o.v);
        break;
    }
    case OP_CLEAR:
        dut->clear_we = 1;
        shim_pcm_clear();
        break;
    case OP_COMMIT:
        dut->commit_we = 1;
        shim_pcm_put(s.staged.data(), (uint32_t)s.staged.size());
        s.staged.clear();
        break;
    }
}

/* One clock: the level compared, the sample on a tick's clock, at most one
 * op, then the edge. */
static const char *clock(sim_t &s)
{
    const long n = s.clock / F, c = s.clock % F;
    if (dut->pcm_level != shim_pcm_level())
    {
        snprintf(why, sizeof why, "clock %ld (sample %ld + %ld): level %u, pcm.c %u", s.clock, n, c,
                 (unsigned)dut->pcm_level, (unsigned)shim_pcm_level());
        return why;
    }
    s.inc = 0;
    s.opened = false;
    const uint32_t under = shim_pcm_underruns();
    if (dut->pcm_under != under)
    {
        snprintf(why, sizeof why, "clock %ld (sample %ld + %ld): underruns %u, pcm.c %u", s.clock, n, c,
                 (unsigned)dut->pcm_under, (unsigned)under);
        return why;
    }
    if (dut->rootp->tb_aud__DOT__pcm__DOT__state == S_SUM && !dut->rootp->tb_aud__DOT__pcm__DOT__so)
    {
        const int ch = dut->rootp->tb_aud__DOT__pcm__DOT__ch;
        const int64_t want = s.want_v[ch] + (1 << (RSMP_Q - 1));
        if ((int64_t)dut->pcm_v != want)
        {
            snprintf(why, sizeof why, "sample %ld channel %d: unrounded pcm.sv %lld, pcm.c %lld (fmt %u,%u,%u)",
                     n, ch, (long long)(int64_t)dut->pcm_v, (long long)want, (unsigned)s.fmt.rate, s.fmt.bits,
                     s.fmt.channels);
            return why;
        }
        s.v_checked++;
    }
    if (c == 0)
    {
        const int32_t l = (int32_t)dut->pcm_l;
        const int32_t r = (int32_t)dut->pcm_r;
        if (l != s.want_l || r != s.want_r)
        {
            snprintf(why, sizeof why, "sample %ld: pcm.sv %d %d, pcm.c %d %d (fmt %u,%u,%u)", n - 1, l, r,
                     s.want_l, s.want_r, (unsigned)s.fmt.rate, s.fmt.bits, s.fmt.channels);
            return why;
        }
        for (const int32_t x : {l, r})
            if (abs(x) > s.peak)
                s.peak = abs(x);
        if (dut->rootp->tb_aud__DOT__pcm__DOT__state != S_IDLE)
            return "a tick found the player busy";
        s.want_l = s.want_r = 0;
        if (s.open)
        {
            const uint64_t ph = dut->rootp->tb_aud__DOT__pcm__DOT__rst_pend
                                    ? 1ull << 32
                                    : dut->rootp->tb_aud__DOT__pcm__DOT__phase;
            const unsigned frame = s.fmt.bits / 8 * s.fmt.channels;
            if (ph >> 32)
                (shim_pcm_level() >= frame ? s.whole : s.dry)++;
            if (shim_pcm_level() == BUF)
                s.brim++;
            shim_pcm_sample(&s.want_l, &s.want_r);
            s.inc = shim_pcm_underruns() - under;
            shim_pcm_v(&s.want_v[0], &s.want_v[1]);
        }
        s.samples++;
        dut->tick = 1;
        dut->step = 1;
    }
    if (!s.q.empty() && s.q.front().at <= s.clock)
    {
        const op_t o = s.q.front();
        s.q.pop_front();
        apply(s, o);
        s.taken_ops++;
        if (c == 0)
            s.on_tick[o.kind]++;
        else if (c <= 110)
            s.in_work[o.kind]++;
    }
    if (s.rsmp && s.clock == s.r_next)
    {
        const int16_t x = r_gen(s.r_i++);
        dut->in_valid = 1;
        dut->in_sample = x;
        int32_t o[8];
        const int got = rsmp_push(&s.r, x, s.r_step, o, 8);
        for (int k = 0; k < got; k++)
            s.rc.push_back((int16_t)(o[k] < -32768 ? -32768 : o[k] > 32767 ? 32767 : o[k]));
        s.r_next += 1014;
    }
    /* A read of the count now and then, which clears it on this clock: an
     * underrun on the same clock leaves one, and an open none. */
    if (rnd() % 997 == 0)
    {
        dut->under_take = 1;
        shim_pcm_set_underruns(s.opened ? 0 : s.inc);
    }
    s.under += s.inc;
    dut->clk = 1;
    dut->eval();
    if (dut->rsmp_valid)
        s.rv.push_back((int16_t)dut->rsmp_out);
    dut->clk = 0;
    dut->tick = dut->step = 0;
    dut->under_take = 0;
    dut->ctrl_we = dut->step_we = dut->data_we = dut->byte_we = dut->clear_we = 0;
    dut->commit_we = 0;
    dut->in_valid = 0;
    dut->eval();
    s.clock++;
    return NULL;
}

static int32_t rails_next(rails_t &p)
{
    if (p.seg-- <= 0)
    {
        p.seg = rr(10, 70);
        p.kind = rr(0, 5);
    }
    p.k++;
    if (p.kind <= 2 && rr(0, 40) == 0)
        p.flip ^= 1;
    switch (p.kind)
    {
    case 3:
        return 32767;
    case 4:
        return -32768;
    case 5:
        return rr(0, 1) ? 32767 : -32768;
    default:
        return ((p.k ^ (uint32_t)p.flip) & 1) ? 32767 : -32768;
    }
}

/* A right channel often repeats its left. */
static uint8_t byte_of(sim_t &s, uint32_t i)
{
    if (!s.rails)
        return (uint8_t)(rnd() ^ i);
    rails_t &p = s.rl;
    const uint32_t in_s = p.pos % p.bps;
    if (in_s == 0 && (p.pos % p.frame == 0 || rr(0, 3)))
        p.val = rails_next(p);
    p.pos++;
    if (p.bps == 1)
        return p.val >= 0 ? 0xFF : 0x00;
    return (uint8_t)((uint16_t)p.val >> (8 * in_s));
}

/* n bytes queued from at as words and single bytes, gap clocks apart, then
 * their commit. */
static void queue_bytes(sim_t &s, long at, uint32_t n, int byte_pct, int gap_lo, int gap_hi)
{
    if (!n)
        return;
    while (n)
    {
        if (n >= 4 && rr(0, 99) >= byte_pct)
        {
            uint32_t v = 0;
            for (int k = 0; k < 4; k++)
                v |= (uint32_t)byte_of(s, n) << (8 * k);
            push(s, at, OP_DATA, v);
            n -= 4;
        }
        else
        {
            push(s, at, OP_BYTE, byte_of(s, n) | (rnd() & 0xFFFFFF00u));
            n -= 1;
        }
        at = s.last + rr(gap_lo, gap_hi);
    }
    push(s, at, OP_COMMIT);
}

/* The clocks where the player changes state, with a push and without. */
static const int edge_c[] = {0, 1, 2, 3, 4, 5, 49, 50, 51, 52, 53, 54, 55,
                             99, 100, 101, 102, 103, 104, 105, 106, 107, F - 1};

/* A clock in frame n, often one the player is working on or the tick's,
 * and with edges half the time one of edge_c. */
static long any_clock(long n, bool edges)
{
    if (edges && rr(0, 1))
        return n * F + edge_c[rr(0, (int)(sizeof edge_c / sizeof edge_c[0]) - 1)];
    switch (rr(0, 5))
    {
    case 0:
        return n * F;
    case 1:
        return n * F + rr(1, 4);
    case 2:
        return n * F + rr(100, 112);
    case 3:
        return n * F + F - 1;
    default:
        return n * F + rr(0, F - 1);
    }
}

struct case_t
{
    std::vector<fmt_t> fmts;
    long frames;
    /* Bytes in a write burst at most, and how often one starts. */
    int chunk;
    int burst_frames;
    int byte_pct;
    int gap_lo, gap_hi;
    /* 1 in so many frames: a clear, a close and reopen with the next
     * format, a pause long enough to run dry, a fill to the brim. */
    int p_clear, p_reopen, p_pause, p_fill;
    /* Frames a pause lasts at most. 1 in p_reopen_open frames: a reopen
     * with no close, the step and the open a clock apart. */
    int pause, p_reopen_open;
    /* Bytes before the first word, so every word lands at this
     * alignment when byte_pct is 0. */
    uint32_t lead;
    /* Ops on edge_c half the time; full-scale samples. */
    bool edges, rails;
    bool rsmp;
    uint32_t seed;
};

static long total_samples, total_v;

static const char *run(const case_t &k, int32_t *peak = NULL)
{
    rs = k.seed;
    fresh();
    /* Empty, as the fabric starts. */
    shim_pcm_reset(8000, 8, 1);
    sim_t s;
    s.rails = k.rails;
    s.rsmp = k.rsmp;
    if (k.rsmp)
    {
        rsmp_reset(&s.r);
        s.r_step = rsmp_step(F, 1014);
    }
    size_t fi = 0;
    push(s, rr(0, F - 1), OP_STEP, 0, k.fmts[0]);
    push(s, s.last + rr(1, 8), OP_OPEN, 0, k.fmts[0]);
    queue_bytes(s, s.last + 1, k.lead, 100, 1, 1);
    long pause_until = 0;
    long next_burst = 0;
    for (long n = 0; n < k.frames || !s.q.empty(); n++)
    {
        if (n < k.frames)
        {
            if (k.p_reopen && rr(1, k.p_reopen) == 1)
            {
                fi = (fi + 1) % k.fmts.size();
                push(s, any_clock(n, k.edges), OP_CLOSE);
                const long at = s.last + (rr(0, 3) ? rr(1, 20) : rr(1, 3 * F));
                push(s, at, OP_STEP, 0, k.fmts[fi]);
                push(s, s.last + rr(1, 20), OP_OPEN, 0, k.fmts[fi]);
            }
            if (k.p_reopen_open && rr(1, k.p_reopen_open) == 1)
            {
                fi = (fi + 1) % k.fmts.size();
                push(s, any_clock(n, k.edges), OP_STEP, 0, k.fmts[fi]);
                push(s, s.last + 1, OP_OPEN, 0, k.fmts[fi]);
            }
            if (k.p_clear && rr(1, k.p_clear) == 1)
                push(s, any_clock(n, k.edges), OP_CLEAR);
            if (k.p_pause && rr(1, k.p_pause) == 1)
                pause_until = n + rr(1, k.pause);
            if (n >= pause_until && n >= next_burst && s.q.size() < 16)
            {
                /* The room as it will be when the burst starts: only the
                 * player and this queue move the level, and the player
                 * only frees room. */
                const bool fill = k.p_fill && rr(1, k.p_fill) == 1;
                long at = any_clock(n, k.edges);
                if (at <= s.last)
                    at = s.last + 1;
                while (s.clock < at)
                    if (const char *e = clock(s))
                        return e;
                uint32_t queued = 0;
                for (const op_t &o : s.q)
                    queued += o.kind == OP_DATA ? 4 : o.kind == OP_BYTE ? 1 : 0;
                const uint32_t room = BUF - shim_pcm_level() - queued - (uint32_t)s.staged.size();
                uint32_t want = fill ? room : (uint32_t)rr(1, k.chunk);
                if (want > room)
                    want = room;
                if (!k.byte_pct)
                    want &= ~3u;
                queue_bytes(s, at, want, k.byte_pct, k.gap_lo, k.gap_hi);
                next_burst = n + rr(0, k.burst_frames);
            }
        }
        while (s.clock < (n + 1) * F)
            if (const char *e = clock(s))
                return e;
    }
    if (const char *e = clock(s))
        return e;
    if (k.rsmp)
    {
        const size_t m = s.rv.size() < s.rc.size() ? s.rv.size() : s.rc.size();
        for (size_t i = 0; i < m; i++)
            if (s.rv[i] != s.rc[i])
            {
                snprintf(why, sizeof why, "rsmp out %zu: rsmp.sv %d, rsmp.c %d", i, s.rv[i], s.rc[i]);
                return why;
            }
        if ((long)m < s.samples / 2)
            return "rsmp made too few samples";
        fprintf(stderr, "  rsmp: %zu samples equal\n", m);
    }
    total_samples += s.samples;
    total_v += s.v_checked;
    if (peak && s.peak > *peak)
        *peak = s.peak;
    fprintf(stderr,
            "  %ld samples equal, %ld unrounded; peak %d; takes %ld whole, %ld dry; %ld underruns; %ld ticks full; %ld ops; "
            "words at head%%4 %u %u %u %u\n"
            "    on a tick / in the filter's clocks: step %ld/%ld open %ld/%ld close %ld/%ld data "
            "%ld/%ld byte %ld/%ld clear %ld/%ld\n",
            s.samples, s.v_checked, s.peak, s.whole, s.dry, s.under, s.brim, s.taken_ops, s.align[0], s.align[1],
            s.align[2], s.align[3],
            s.on_tick[OP_STEP], s.in_work[OP_STEP], s.on_tick[OP_OPEN], s.in_work[OP_OPEN],
            s.on_tick[OP_CLOSE], s.in_work[OP_CLOSE], s.on_tick[OP_DATA], s.in_work[OP_DATA],
            s.on_tick[OP_BYTE], s.in_work[OP_BYTE], s.on_tick[OP_CLEAR], s.in_work[OP_CLEAR]);
    return NULL;
}

#define ASSERT_OK(x)                              \
    do                                            \
    {                                             \
        const char *why_ = (x);                   \
        ASSERT_TRUE_MSG(!why_, why_ ? why_ : ""); \
    } while (0)

static const uint32_t rates[] = {8000, 11025, 22050, 32000, 44100, 47999, 48000};

/* Every rate in one format, kept fed, with clears and the odd run dry. */
static const char *rates_in(uint8_t bits, uint8_t channels, uint32_t seed)
{
    for (uint32_t rate : rates)
    {
        case_t k = {};
        k.fmts = {{rate, bits, channels}};
        k.frames = 2500;
        k.chunk = 700;
        k.burst_frames = 40;
        k.byte_pct = 30;
        k.gap_lo = 1;
        k.gap_hi = 6;
        k.p_clear = 500;
        k.p_pause = 1500;
        k.pause = 400;
        k.seed = seed + rate;
        fprintf(stderr, " %u,%u,%u\n", (unsigned)rate, bits, channels);
        if (const char *e = run(k))
            return e;
    }
    return NULL;
}

UTEST(pcm_rtl, rates_8_mono) { ASSERT_OK(rates_in(8, 1, 0x1234)); }
UTEST(pcm_rtl, rates_8_stereo) { ASSERT_OK(rates_in(8, 2, 0x2345)); }
UTEST(pcm_rtl, rates_16_mono) { ASSERT_OK(rates_in(16, 1, 0x3456)); }
UTEST(pcm_rtl, rates_16_stereo) { ASSERT_OK(rates_in(16, 2, 0x4567)); }

/* Filled to the brim again and again, written a clock apart. */
UTEST(pcm_rtl, full_buffer)
{
    case_t k = {};
    k.fmts = {{48000, 16, 2}, {44100, 16, 1}, {32000, 8, 2}};
    k.frames = 12000;
    k.chunk = 4096;
    k.burst_frames = 2;
    k.byte_pct = 10;
    k.gap_lo = 1;
    k.gap_hi = 1;
    k.p_fill = 1;
    k.p_reopen = 3000;
    k.seed = 0xF111;
    ASSERT_OK(run(k));
}

/* Starved: short bursts that leave it dry between them. */
UTEST(pcm_rtl, underrun_and_resume)
{
    case_t k = {};
    k.fmts = {{48000, 16, 2}, {22050, 8, 1}, {47999, 16, 1}, {11025, 8, 2}};
    k.frames = 16000;
    k.chunk = 40;
    k.burst_frames = 120;
    k.byte_pct = 50;
    k.gap_lo = 1;
    k.gap_hi = 30;
    k.p_pause = 300;
    k.pause = 600;
    k.p_reopen = 4000;
    k.seed = 0xD12;
    ASSERT_OK(run(k));
}

/* Every op on any clock: clears, closes and reopens in another format
 * landing on the tick, in the filter's work and between. */
UTEST(pcm_rtl, events_on_any_clock)
{
    case_t k = {};
    k.fmts = {{8000, 8, 1}, {48000, 16, 2}, {22050, 16, 1}, {44100, 8, 2},
              {32000, 16, 2}, {47999, 8, 1}, {11025, 16, 2}};
    k.frames = 30000;
    k.chunk = 300;
    k.burst_frames = 30;
    k.byte_pct = 40;
    k.gap_lo = 1;
    k.gap_hi = 400;
    k.p_clear = 40;
    k.p_reopen = 150;
    k.p_reopen_open = 150;
    k.p_pause = 800;
    k.pause = 300;
    k.p_fill = 50;
    k.seed = 0xACE;
    ASSERT_OK(run(k));
}

/* Words at every alignment across the wrap, at the bus's pace. */
UTEST(pcm_rtl, words_across_the_wrap)
{
    for (int lead = 0; lead < 4; lead++)
    {
        case_t k = {};
        k.fmts = {{48000, 16, 2}};
        k.frames = 4000;
        k.chunk = 1024;
        k.burst_frames = 100;
        k.byte_pct = 0;
        k.lead = (uint32_t)lead;
        k.gap_lo = 4;
        k.gap_hi = 4;
        k.seed = 0x5EED + (uint32_t)lead;
        ASSERT_OK(run(k));
    }
}

/* Full-scale samples at rates whose rows reach 64, where the filter
 * peaks: the dot, the difference and the output each need their every
 * bit, and the output passes 17 bits. */
static const char *rails_in(uint8_t bits, uint8_t channels, uint32_t seed)
{
    static const uint32_t rails_rates[] = {1, 8000, 11025, 12000, 16000, 22050,
                                           24000, 32000, 36000, 44100, 47999, 48000};
    int32_t peak = 0;
    for (uint32_t rate : rails_rates)
    {
        case_t k = {};
        k.fmts = {{rate, bits, channels}};
        k.frames = rate < 4000 ? 600 : 2500;
        k.chunk = 700;
        k.burst_frames = 40;
        k.gap_lo = 1;
        k.gap_hi = 6;
        k.rails = true;
        k.seed = seed + rate;
        fprintf(stderr, " %u,%u,%u\n", (unsigned)rate, bits, channels);
        if (const char *e = run(k, &peak))
            return e;
    }
    if (peak <= 65535)
        return "the rails never passed 17 bits";
    return NULL;
}

UTEST(pcm_rtl, rails_8_mono) { ASSERT_OK(rails_in(8, 1, 0x91)); }
UTEST(pcm_rtl, rails_8_stereo) { ASSERT_OK(rails_in(8, 2, 0x92)); }
UTEST(pcm_rtl, rails_16_mono) { ASSERT_OK(rails_in(16, 1, 0x93)); }
UTEST(pcm_rtl, rails_16_stereo) { ASSERT_OK(rails_in(16, 2, 0x94)); }

/* Any rate from 1 to 48000 in any format. */
UTEST(pcm_rtl, any_rate)
{
    for (int i = 0; i < 40; i++)
    {
        rs = 0x777 + (uint32_t)i;
        const uint32_t rate = i < 4   ? (uint32_t)(i + 1)
                              : i < 8 ? 48000 - (uint32_t)(i - 4)
                                      : (uint32_t)rr(1, 48000);
        case_t k = {};
        k.fmts = {{rate, (uint8_t)(rr(0, 1) ? 16 : 8), (uint8_t)rr(1, 2)}};
        k.frames = 1200;
        k.chunk = 500;
        k.burst_frames = 30;
        k.byte_pct = 30;
        k.gap_lo = 1;
        k.gap_hi = 6;
        k.p_clear = 300;
        k.p_pause = 600;
        k.pause = 200;
        k.seed = 0xA000 + (uint32_t)i;
        fprintf(stderr, " %u,%u,%u\n", (unsigned)rate, k.fmts[0].bits, k.fmts[0].channels);
        ASSERT_OK(run(k));
    }
}

/* Every op on the clocks where the player changes state. */
UTEST(pcm_rtl, events_on_the_edges)
{
    case_t k = {};
    k.fmts = {{8000, 8, 1}, {48000, 16, 2}, {22050, 16, 1}, {44100, 8, 2},
              {24000, 16, 2}, {47999, 8, 1}, {11025, 16, 2}, {48000, 8, 2}};
    k.frames = 40000;
    k.chunk = 64;
    k.burst_frames = 3;
    k.byte_pct = 60;
    k.gap_lo = 1;
    k.gap_hi = 3;
    k.p_clear = 30;
    k.p_reopen = 100;
    k.p_reopen_open = 100;
    k.p_pause = 400;
    k.pause = 50;
    k.p_fill = 80;
    k.edges = true;
    k.seed = 0xED6E;
    ASSERT_OK(run(k));
}

/* A frame or less at a time, so frames complete on every clock, the
 * tick's included, and the FIFO often holds part of one. */
UTEST(pcm_rtl, trickle)
{
    const fmt_t f[] = {{48000, 16, 2}, {47999, 16, 1}, {44100, 8, 2}, {24000, 16, 2}};
    for (int i = 0; i < 4; i++)
    {
        case_t k = {};
        k.fmts = {f[i]};
        k.frames = 8000;
        k.chunk = 3;
        k.burst_frames = 1;
        k.byte_pct = 100;
        k.gap_lo = 1;
        k.gap_hi = 700;
        k.p_clear = 60;
        k.edges = true;
        k.seed = 0x7111 + (uint32_t)i;
        ASSERT_OK(run(k));
    }
}

/* Closes on the tick's clock and the push's, and never a reopen of an
 * open device: the frame taken on a close's tick plays in the format it
 * was taken in. */
UTEST(pcm_rtl, close_on_the_tick)
{
    case_t k = {};
    k.fmts = {{44100, 16, 2}, {32000, 16, 1}, {22050, 8, 2}, {47999, 16, 2}};
    k.frames = 30000;
    k.chunk = 400;
    k.burst_frames = 10;
    k.byte_pct = 20;
    k.gap_lo = 1;
    k.gap_hi = 8;
    k.p_reopen = 40;
    k.edges = true;
    k.seed = 0xC105E;
    ASSERT_OK(run(k));
}

/* Full-scale samples through reopens in other formats, clears and
 * fills, on the edges. */
UTEST(pcm_rtl, rails_and_events)
{
    case_t k = {};
    k.fmts = {{24000, 16, 2}, {47999, 16, 1}, {32000, 8, 2}, {44100, 16, 2}, {16000, 8, 1}};
    k.frames = 30000;
    k.chunk = 400;
    k.burst_frames = 20;
    k.byte_pct = 20;
    k.gap_lo = 1;
    k.gap_hi = 8;
    k.p_clear = 200;
    k.p_reopen = 300;
    k.p_reopen_open = 300;
    k.p_fill = 60;
    k.edges = true;
    k.rails = true;
    k.seed = 0xE7E7;
    ASSERT_OK(run(k));
}

/* rsmp on the ROM's other port while PCM plays. */
UTEST(pcm_rtl, beside_rsmp)
{
    case_t k = {};
    k.fmts = {{44100, 16, 2}, {48000, 8, 1}};
    k.frames = 6000;
    k.chunk = 500;
    k.burst_frames = 60;
    k.byte_pct = 25;
    k.gap_lo = 1;
    k.gap_hi = 10;
    k.p_reopen = 2000;
    k.rsmp = true;
    k.seed = 0xB0B;
    ASSERT_OK(run(k));
    fprintf(stderr, "  %ld samples compared in all, %ld unrounded\n", total_samples, total_v);
}
