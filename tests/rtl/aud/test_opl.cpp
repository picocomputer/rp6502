/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * opl.sv against YM3812-LLE, nukeykt's model of the die and the oracle,
 * sample for sample on every script test_opl.c runs on the die. Against
 * opl.c in lockstep on everything, the bursts the die cannot take included:
 * every write, map and drop read goes to both in the same sample interval,
 * and every sample is compared, as are the queue's count, the credit and the
 * late half before each one.
 */

#include "Vopl.h"
#include "Vopl___024root.h"

#include "utest.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

extern "C"
{
#include "core/aud/opl.h"
#include "opl_lle.h"
#include "opl_script.h"
}

/* opl.sv's fstate. */
enum
{
    F_IDLE,
    F_CLEAR,
    F_LAG,
    F_POP
};
#define TICKS 1014

static Vopl *dut;
/* Every frame's opl_out, and the frames that have passed their pop. */
static std::vector<int16_t> outs;
static size_t frames;

static int fstate() { return dut->rootp->opl__DOT__fstate; }
static int tickctr() { return dut->rootp->opl__DOT__tickctr; }

/* The clock a frame pops on, after the clear when a pointer write came
 * before it. An event on an earlier clock is before that frame's sample,
 * and one on this clock or later after it. */
static bool at_pop() { return fstate() == F_POP && !dut->rootp->opl__DOT__rst_pend; }

static void tick()
{
    if (at_pop())
        frames++;
    dut->clk = 1;
    dut->eval();
    if (dut->opl_valid)
        outs.push_back((int16_t)dut->opl_out);
    dut->clk = 0;
    dut->eval();
    dut->q_we = 0;
    dut->xaddr_we = 0;
    dut->drops_take = 0;
}

enum
{
    EV_WRITE,
    /* A write and a drop read on one clock. */
    EV_WRITE_DROPS,
    EV_MAP,
    EV_DROPS,
    /* Both drop counts set to NEAR_SAT. */
    EV_NEAR_SAT
};
#define NEAR_SAT 0x7FFFFFF0u

struct opl_ev
{
    uint32_t n;
    int kind;
    uint16_t word;
    uint8_t val;
    /* The tickctr the event lands on, or -1 for the next clock. */
    int clk;
};

static char why[640];
/* The loudest sample of the last lockstep. */
static int peak;
static std::vector<opl_ev> evs;
/* YM3812-LLE's samples for evs, when the die can take them: the oracle,
 * which lockstep holds opl.sv to before opl.c. */
static std::vector<int16_t> die;

static void ev_map(uint32_t n, uint16_t word, int clk = -1) { evs.push_back({n, EV_MAP, word, 0, clk}); }
static void ev_write(uint32_t n, uint16_t addr, uint8_t val, int clk = -1)
{
    evs.push_back({n, EV_WRITE, addr, val, clk});
}
static void ev_write_drops(uint32_t n, uint16_t addr, uint8_t val, int clk = -1)
{
    evs.push_back({n, EV_WRITE_DROPS, addr, val, clk});
}
static void ev_drops(uint32_t n, int clk = -1) { evs.push_back({n, EV_DROPS, 0, 0, clk}); }
static void ev_near_sat(uint32_t n) { evs.push_back({n, EV_NEAR_SAT, 0, 0, -1}); }

static uint32_t rs = 1;
static uint32_t rnd()
{
    rs ^= rs << 13;
    rs ^= rs >> 17;
    rs ^= rs << 5;
    return rs;
}
static int rr(int lo, int hi) { return lo + (int)(rnd() % (uint32_t)(hi - lo + 1)); }

/* A script's pushes to 256 bytes starting at page * 257, so that only
 * page 0 starts a page, mapped before sample 0, with the drops read every
 * 1000 samples and at the end. */
static void ev_script(const opl_script_t *s, uint8_t page)
{
    const uint16_t at = (uint16_t)(page << 8 | page);
    evs.clear();
    ev_map(0, at);
    size_t i = 0;
    for (uint32_t n = 0; n < s->samples; n++)
    {
        for (; i < s->count && s->w[i].n == n; i++)
            ev_write(n, (uint16_t)(at + s->w[i].reg), s->w[i].val);
        if (n % 1000 == 999 || n + 1 == s->samples)
            ev_drops(n);
    }
}

/* Puts e on the inputs for the next clock, and gives it to opl.c. */
static const char *inject(const opl_ev &e, uint16_t *at, uint32_t n)
{
    if (e.kind == EV_DROPS || e.kind == EV_WRITE_DROPS)
    {
        const uint32_t r = dut->opl_drops, c = opl_take_drops();
        dut->drops_take = 1;
        if (r != c)
        {
            snprintf(why, sizeof why, "sample %u: opl.sv dropped %u, opl.c %u", (unsigned)n, (unsigned)r,
                     (unsigned)c);
            return why;
        }
    }
    if (e.kind == EV_WRITE || e.kind == EV_WRITE_DROPS)
    {
        dut->q_we = 1;
        dut->q_addr = e.word;
        dut->q_val = e.val;
        if (*at != 0xFFFF && (uint16_t)(e.word - *at) < OPL_REGS_SIZE)
            opl_push((uint8_t)(e.word - *at), e.val);
    }
    else if (e.kind == EV_MAP)
    {
        dut->xaddr_we = 1;
        dut->xaddr_wdata = e.word;
        opl_map(e.word);
        *at = e.word;
    }
    else if (e.kind == EV_NEAR_SAT)
    {
        dut->rootp->opl_drops = NEAR_SAT;
        opl_queue.drops = NEAR_SAT;
    }
    return NULL;
}

/* NULL when opl.sv and opl.c agree on every sample of evs, else the first
 * disagreement. Events whose sample interval closes before they are all
 * out spill into the next interval on both sides alike. With jitter, the
 * first event of each interval waits up to that many clocks, so the
 * events land on any clock of the frame. */
static const char *lockstep(uint32_t samples, uint32_t jitter)
{
    long guard = 0;
    while (!at_pop() && guard++ < 4 * TICKS)
        tick();
    tick();
    uint16_t at = 0xFFFF;
    std::vector<int16_t> want(samples);
    std::vector<size_t> frame(samples);
    size_t i = 0;
    for (uint32_t n = 0; n < samples; n++)
    {
        uint32_t wait = jitter ? rnd() % jitter : 0;
        while (i < evs.size() && evs[i].n <= n)
        {
            const opl_ev &e = evs[i];
            while (!at_pop() && (e.clk >= 0 ? tickctr() != e.clk : wait > 0))
            {
                if (e.clk < 0)
                    wait--;
                tick();
            }
            if (at_pop())
                break;
            i++;
            if (const char *r = inject(e, &at, n))
                return r;
            tick();
        }
        guard = 0;
        while (!at_pop() && guard++ < 4 * TICKS)
            tick();
        if (guard >= 4 * TICKS)
            return "no frame reached its pop";
        frame[n] = frames;
        /* An event due on the pop's own clock is after the sample, but a
         * write with the queue full goes before it, where the full queue
         * drops it as opl.sv does; one the engine ignores lands alike. */
        bool due = i < evs.size() && evs[i].n <= n + 1 && evs[i].clk == tickctr();
        if (due && (evs[i].kind == EV_WRITE || evs[i].kind == EV_WRITE_DROPS)
            && dut->rootp->opl__DOT__q_len == OPL_QUEUE_SIZE)
        {
            if (const char *r = inject(evs[i++], &at, n))
                return r;
            due = false;
        }
        if (at == 0xFFFF)
            want[n] = 0;
        else
        {
            uint32_t credit;
            bool late;
            opl_pace_get(&credit, &late);
            const unsigned rq = dut->rootp->opl__DOT__q_len;
            const unsigned rc = dut->rootp->opl__DOT__credit * 24u;
            const bool rl = dut->rootp->opl__DOT__late;
            if (rq != RING_LEN(&opl_queue.ring) || rc != credit || rl != late)
            {
                snprintf(why, sizeof why,
                         "before sample %u: opl.sv queue %u credit %u late %d, opl.c %u %u %d",
                         (unsigned)n, rq, rc, rl, (unsigned)RING_LEN(&opl_queue.ring), (unsigned)credit, late);
                return why;
            }
            want[n] = opl_sample();
        }
        if (due)
            if (const char *r = inject(evs[i++], &at, n + 1))
                return r;
        tick();
    }
    guard = 0;
    while (outs.size() <= frame[samples - 1] && guard++ < 4 * TICKS)
        tick();
    peak = 0;
    for (uint32_t n = 0; n < samples; n++)
    {
        const int16_t got = outs.at(frame[n]);
        if (std::abs(want[n]) > peak)
            peak = std::abs(want[n]);
        const bool off_die = !die.empty() && got != die[n];
        if (!off_die && got == want[n])
            continue;
        int len = snprintf(why, sizeof why, "sample %u: opl.sv %d, %s %d; events up to it:",
                           (unsigned)n, got, off_die ? "LLE" : "opl.c", off_die ? die[n] : want[n]);
        size_t last = 0;
        while (last < evs.size() && evs[last].n <= n)
            last++;
        for (size_t k = last > 8 ? last - 8 : 0; k < last && len < (int)sizeof why - 32; k++)
            len += snprintf(why + len, sizeof why - (size_t)len, " %u:%d:%04X=%02X@%d",
                            (unsigned)evs[k].n, evs[k].kind, evs[k].word, evs[k].val, evs[k].clk);
        return why;
    }
    return NULL;
}

/* lockstep, with the samples it compared reported. */
static const char *compare(uint32_t samples, uint32_t jitter = 0)
{
    const char *r = lockstep(samples, jitter);
    if (!r)
        fprintf(stderr, "  %u samples equal to opl.c%s, peak %d\n", (unsigned)samples,
                die.empty() ? "" : " and LLE", peak);
    return r;
}

/* The die on s's pushes, paced as the chip allows, as test_opl.c runs it;
 * with bus, the pushes as they stand, every write that is not skipped then
 * applying as it is pushed. */
static const char *lle_on(const opl_script_t *s, bool bus)
{
    std::vector<uint32_t> at(s->count + 1);
    opl_script_t schedule = {};
    opl_pace(s, at.data(), &schedule);
    const char *r = NULL;
    for (size_t i = 0; bus && i < s->count && !r; i++)
        if (at[i] != OPL_SKIPPED && at[i] != s->w[i].n)
        {
            snprintf(why, sizeof why, "write %zu (%02X=%02X at %u) is not applied as pushed", i,
                     s->w[i].reg, s->w[i].val, (unsigned)s->w[i].n);
            r = why;
        }
    die.resize(s->samples);
    if (!r)
        r = lle_run(bus ? s : &schedule, 5, die.data());
    opl_script_free(&schedule);
    return r;
}

enum
{
    NO_DIE,
    DIE_PACED,
    DIE_BUS
};

static const char *script_free(opl_script_t *s, uint8_t page, uint32_t jitter = 0, int oracle = NO_DIE)
{
    ev_script(s, page);
    die.clear();
    const char *r = oracle == NO_DIE ? NULL : lle_on(s, oracle == DIE_BUS);
    const uint32_t samples = s->samples;
    opl_script_free(s);
    if (!r)
        r = compare(samples, jitter);
    die.clear();
    return r;
}

#define ASSERT_OK(x)                              \
    do                                            \
    {                                             \
        const char *why_ = (x);                   \
        ASSERT_TRUE_MSG(!why_, why_ ? why_ : ""); \
    } while (0)

/* A script of tests/host/emu on page. */
#define SCRIPT(call, page)                \
    do                                    \
    {                                     \
        opl_script_t s = {};              \
        call;                             \
        ASSERT_OK(script_free(&s, page)); \
    } while (0)

/* One test_opl.c runs on the die, held to the die as well. */
#define LLE(call, page)                                 \
    do                                                  \
    {                                                   \
        opl_script_t s = {};                            \
        call;                                           \
        ASSERT_OK(script_free(&s, page, 0, DIE_PACED)); \
    } while (0)

UTEST(opl, power_on) { LLE(opl_script_power_on(&s), 0x12); }
UTEST(opl, envelopes) { LLE(opl_script_envelopes(&s), 0x40); }
UTEST(opl, levels) { LLE(opl_script_levels(&s), 0xF0); }
UTEST(opl, waves) { LLE(opl_script_waves(&s), 0x00); }
UTEST(opl, feedback) { LLE(opl_script_feedback(&s), 0x12); }
UTEST(opl, lfo) { LLE(opl_script_lfo(&s), 0x12); }
UTEST(opl, rhythm) { LLE(opl_script_rhythm(&s), 0x12); }
UTEST(opl, retrigger) { LLE(opl_script_retrigger(&s), 0x12); }
UTEST(opl, clip) { LLE(opl_script_clip(&s, false), 0x12); }
UTEST(opl, clip_rhythm) { LLE(opl_script_clip(&s, true), 0x12); }
UTEST(opl, test_bits_1) { LLE(opl_script_test_bits(&s, 1), 0x12); }
UTEST(opl, test_bits_2) { LLE(opl_script_test_bits(&s, 2), 0x12); }
UTEST(opl, keyoff_silent) { LLE(opl_script_keyoff_silent(&s), 0x12); }
UTEST(opl, decay_sl0) { LLE(opl_script_decay_sl0(&s), 0x12); }
UTEST(opl, eg_wrap) { LLE(opl_script_eg_wrap(&s), 0x12); }
UTEST(opl, random) { LLE(opl_script_random(&s, 1, 8000, 0), 0x12); }
UTEST(opl, random_rhythm) { LLE(opl_script_random(&s, 2, 8000, OPL_RHYTHM), 0x12); }
UTEST(opl, random_alias) { LLE(opl_script_random(&s, 3, 8000, OPL_RHYTHM | OPL_ALIAS), 0x12); }
UTEST(opl, random_slow) { LLE(opl_script_random(&s, 4, 8000, OPL_RHYTHM | OPL_SLOW_ENV), 0x12); }
UTEST(opl, random_test_bits) { LLE(opl_script_random(&s, 8, 8000, OPL_RHYTHM | OPL_TEST_BITS), 0x12); }
UTEST(opl, dense) { LLE(opl_script_dense(&s, 5, 8000, OPL_RHYTHM), 0x12); }
UTEST(opl, dense_alias) { LLE(opl_script_dense(&s, 6, 8000, OPL_RHYTHM | OPL_ALIAS), 0x12); }
UTEST(opl, dense_slow) { LLE(opl_script_dense(&s, 7, 8000, OPL_SLOW_ENV | OPL_GAP2), 0x12); }
UTEST(opl, dense_test_bits) { LLE(opl_script_dense(&s, 9, 8000, OPL_RHYTHM | OPL_TEST_BITS), 0x12); }
UTEST(opl, bursts_1) { SCRIPT(opl_script_bursts(&s, 1, 30000, 0), 0x12); }
UTEST(opl, bursts_2) { SCRIPT(opl_script_bursts(&s, 2, 30000, 0), 0x12); }
UTEST(opl, bursts_3) { SCRIPT(opl_script_bursts(&s, 3, 30000, 0), 0x12); }
UTEST(opl, bursts_test_bits) { SCRIPT(opl_script_bursts(&s, 4, 30000, OPL_TEST_BITS), 0x12); }
UTEST(opl, flood) { SCRIPT(opl_script_flood(&s, 1), 0x12); }

/* Every offset the engine skips, written between the paced writes. */
UTEST(opl, holes)
{
    opl_script_t src = {}, s = {};
    opl_script_random(&src, 10, 8000, OPL_RHYTHM | OPL_ALIAS);
    opl_script_holes(&s, &src, 10);
    opl_script_free(&src);
    ASSERT_OK(script_free(&s, 0x12, 0, DIE_BUS));
}

/* Bursts and a flood landing on any clock of the frame: the tick, the view
 * copies and the pop's own clock among them. */
UTEST(opl, bursts_any_clock)
{
    rs = 0x2545F491;
    opl_script_t s = {};
    opl_script_bursts(&s, 5, 30000, 0);
    ASSERT_OK(script_free(&s, 0x12, TICKS));
}
UTEST(opl, flood_any_clock)
{
    rs = 0x9E3779B9;
    opl_script_t s = {};
    opl_script_flood(&s, 2);
    ASSERT_OK(script_free(&s, 0x12, TICKS));
}

/* 300 writes to one register before one sample: 44 drop. The pacing
 * rule's own cases follow: a channel 0 write's late half in a burst, a
 * run of channel 0-2 writes, and the aliases and C3-C5, which hold
 * nothing back. */
UTEST(opl, pacing)
{
    evs.clear();
    ev_map(0, 0x3000);
    for (int k = 0; k < 300; k++)
        ev_write(10, 0x3040, (uint8_t)k);
    ev_drops(10);
    for (int k = 0; k < 5; k++)
        ev_write(600, (uint16_t)(k == 2 ? 0x30A0 : 0x3041), (uint8_t)k);
    for (int k = 0; k < 7; k++)
        ev_write(700, (uint16_t)(k == 1 ? 0x30B0 : 0x3042), (uint8_t)k);
    static const uint8_t ch02[15] = {0xA0, 0xA1, 0xA2, 0xAE, 0xAF, 0xB0, 0xB1, 0xB2,
                                     0xBE, 0xBF, 0xC0, 0xC1, 0xC2, 0xCE, 0xCF};
    for (int k = 0; k < 15; k++)
        ev_write(800, (uint16_t)(0x3000 | ch02[k]), (uint8_t)(k | 0x20));
    for (int k = 0; k < 8; k++)
        ev_write(800, 0x3042, (uint8_t)k);
    static const uint8_t unheld[12] = {0xA9, 0xAA, 0xAB, 0xB9, 0xBA, 0xBB,
                                       0xC9, 0xCA, 0xCB, 0xC3, 0xC4, 0xC5};
    for (int k = 0; k < 19; k++)
        ev_write(900, (uint16_t)(0x3000 | (k % 3 ? unheld[k - k / 3 - 1] : 0x42)), (uint8_t)k);
    ev_drops(1999);
    ASSERT_OK(compare(2000));
}

/* The drop count saturates, a read on the clock of a drop returns the
 * count before it and leaves that drop counted, and a pointer write clears
 * it. Writes on the pop's own clock find the queue full or not. */
UTEST(opl, drop_count)
{
    evs.clear();
    ev_map(0, 0x3000);
    ev_near_sat(5);
    for (int k = 0; k < 300; k++)
        ev_write(10, 0x3040, (uint8_t)k);
    ev_drops(10);
    ev_drops(11);
    for (int k = 0; k < 300; k++)
        ev_write(400, 0x3041, (uint8_t)k);
    for (int k = 0; k < 6; k++)
        ev_write_drops(400, 0x3041, (uint8_t)k);
    ev_drops(401);
    for (uint32_t n = 402; n < 420; n++)
    {
        ev_write(n, 0x3041, (uint8_t)n);
        ev_write(n, 0x3041, (uint8_t)n);
        if (n & 1)
            ev_write(n, 0x3041, (uint8_t)n, 13);
        else
            ev_write_drops(n, 0x3041, (uint8_t)n, 13);
    }
    for (uint32_t n = 700; n < 720; n++)
        ev_write(n, 0x3043, (uint8_t)n, 13);
    ev_drops(721);
    for (int k = 0; k < 300; k++)
        ev_write(800, 0x3042, (uint8_t)k);
    ev_map(801, 0x3100);
    ev_drops(802);
    ASSERT_OK(compare(1200));
}

/* A note on channel c of page pg. */
static void ev_note(uint32_t n, uint16_t pg, int c)
{
    static const uint8_t mod[9] = {0, 1, 2, 8, 9, 10, 16, 17, 18};
    const int m = mod[c], k = m + 3;
    const int w[11][2] = {{0x20 + m, 0x01}, {0x20 + k, 0x01}, {0x40 + m, 0x10}, {0x40 + k, 0x00},
                          {0x60 + m, 0xF0}, {0x60 + k, 0xF0}, {0x80 + m, 0x77}, {0x80 + k, 0x77},
                          {0xC0 + c, 0x0E}, {0xA0 + c, 0x98}, {0xB0 + c, 0x31}};
    for (const auto &x : w)
        ev_write(n, (uint16_t)(pg | x[0]), (uint8_t)x[1]);
}

/* A pointer write lands on clock clk of the frame, with a channel 1 late
 * half applied the sample before, a write waiting and a channel 0 late half
 * behind it, then a note on the new page. Up to the pop, the tick and the
 * view copies included, it is before the sample; on the pop's clock or
 * later, after it. */
static const char *map_on(int clk, uint16_t to)
{
    evs.clear();
    ev_map(0, 0x1200);
    ev_note(1, 0x1200, 0);
    ev_note(1, 0x1200, 3);
    ev_write(599, 0x12A1, 0x44);
    ev_write(600, 0x124B, 0x3F);
    ev_write(600, 0x12A0, 0x55);
    ev_map(600, to, clk);
    if (to != 0xFFFF)
        ev_note(600, to, 1);
    ev_drops(1199);
    return compare(1200);
}
UTEST(opl, map_by_the_pop)
{
    static const int clks[] = {TICKS - 2, TICKS - 1, 0, 1, 6, 11, 12, 13, 14, 300};
    for (int c : clks)
    {
        ASSERT_OK(map_on(c, 0x3400));
        ASSERT_OK(map_on(c, 0x1200));
        ASSERT_OK(map_on(c, 0xFFFF));
    }
}

/* aud_restore's park then map. The park resets the next frame, and a map
 * that lands in that frame's clear, or on the pop that starts it, joins
 * the reset, so the writes after it apply from that sample on. A map on
 * the pop after the clear is the next frame's. The last two runs land
 * pointer writes 280 clocks apart, and on each clock a reset pop would
 * take: a frame that cleared again for each would outlast its tick. */
UTEST(opl, park_then_map_in_the_clear)
{
    static const int clks[][3] = {{13}, {14}, {15}, {141}, {268}, {269}, {270}, {271},
                                  {20, 300, 580}, {13, 270, 527}};
    static const uint16_t to[3] = {0x1200, 0x3400, 0xFFFF};
    for (const auto &run : clks)
    {
        evs.clear();
        ev_map(0, 0x1200);
        ev_note(1, 0x1200, 0);
        ev_note(1, 0x1200, 4);
        ev_map(600, 0xFFFF, TICKS - 40);
        const int maps = run[1] ? 3 : 1;
        for (int k = 0; k < maps; k++)
            ev_map(600, k + 1 == maps ? 0x1200 : to[k + 1], run[k]);
        ev_note(600, 0x1200, 0);
        ev_note(600, 0x1200, 4);
        ev_drops(1199);
        ASSERT_OK(compare(1200));
    }
}

/* A note, then a remap mid-note, a park with writes that go nowhere, a
 * map back, and writes to a page that is not the mapped one. */
static void ev_remaps(uint32_t seed)
{
    opl_script_t s = {};
    opl_script_random(&s, seed, 6000, OPL_RHYTHM);
    evs.clear();
    ev_map(0, 0x1200);
    size_t i = 0;
    uint8_t page = 0x12;
    for (uint32_t n = 0; n < s.samples; n++)
    {
        if (n == 1500)
            ev_map(n, 0x3400), page = 0x34;
        if (n == 3000)
            ev_map(n, 0xFFFF), page = 0x34;
        if (n == 3400)
            ev_map(n, 0x1200), page = 0x12;
        for (; i < s.count && s.w[i].n == n; i++)
        {
            ev_write(n, (uint16_t)(page << 8 | s.w[i].reg), s.w[i].val);
            ev_write(n, (uint16_t)((page ^ 0x01) << 8 | s.w[i].reg), (uint8_t)~s.w[i].val);
        }
        if (n % 500 == 499)
            ev_drops(n);
    }
    opl_script_free(&s);
}
UTEST(opl, map_park_remap)
{
    ev_remaps(21);
    ASSERT_OK(compare(6000));
}
UTEST(opl, map_park_remap_any_clock)
{
    rs = 0x6C078965;
    ev_remaps(22);
    ASSERT_OK(compare(6000, TICKS));
}

/* Parked from power-on: writes anywhere make no sound. */
UTEST(opl, silent_until_mapped)
{
    evs.clear();
    ev_map(0, 0xFFFF);
    static const uint8_t note[11][2] = {{0x20, 0x01}, {0x23, 0x01}, {0x40, 0x10}, {0x43, 0x00},
                                        {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
                                        {0xC0, 0x0E}, {0xA0, 0x98}, {0xB0, 0x31}};
    for (int k = 0; k < 11; k++)
        ev_write(4, (uint16_t)(0x1200 | note[k][0]), note[k][1]);
    ASSERT_OK(compare(600));
    ASSERT_FALSE(dut->opl_enabled);
}

static const uint8_t opoff[18] = {0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20, 21};
static int modoff(int c) { return opoff[(c / 3) * 6 + c % 3]; }

/* A loud patch on every slot, with every channel keyed. */
static void ev_all(uint16_t pg)
{
    ev_write(1, (uint16_t)(pg | 0x01), 0x20);
    for (int c = 0; c < 9; c++)
    {
        for (int i = 0; i < 2; i++)
        {
            const int off = modoff(c) + 3 * i;
            ev_write(1, (uint16_t)(pg | (0x20 + off)), (uint8_t)(rr(0, 3) << 6 | rr(0, 1) << 5 | rr(0, 1) << 4 | rr(0, 15)));
            ev_write(1, (uint16_t)(pg | (0x40 + off)), (uint8_t)(rr(0, 3) << 6 | (i ? rr(0, 8) : rr(0, 40))));
            ev_write(1, (uint16_t)(pg | (0x60 + off)), (uint8_t)(rr(8, 15) << 4 | rr(0, 15)));
            ev_write(1, (uint16_t)(pg | (0x80 + off)), (uint8_t)(rr(0, 15) << 4 | rr(0, 15)));
            ev_write(1, (uint16_t)(pg | (0xE0 + off)), (uint8_t)rr(0, 3));
        }
        ev_write(1, (uint16_t)(pg | (0xC0 + c)), (uint8_t)rr(0, 15));
        ev_write(1, (uint16_t)(pg | (0xA0 + c)), (uint8_t)rr(0, 255));
        ev_write(1, (uint16_t)(pg | (0xB0 + c)), (uint8_t)(0x20 | rr(0, 31)));
    }
}

/* Any offset and any value at any density, on any clock, with drop reads
 * on the clock of a write, and with maps the pointer moves among two
 * pages and parked. */
static void ev_storm(uint32_t seed, uint32_t samples, bool maps)
{
    rs = seed * 2654435761u + 1;
    evs.clear();
    uint16_t pg = 0x2200, last = pg;
    ev_map(0, pg);
    ev_all(pg);
    for (uint32_t n = 300; n < samples; n++)
    {
        if (maps && rr(0, 1499) == 0)
        {
            static const uint16_t to[3] = {0x2200, 0x5500, 0xFFFF};
            pg = to[rr(0, 2)];
            ev_map(n, pg, rr(0, 3) ? -1 : rr(0, TICKS - 1));
            if (pg != 0xFFFF)
                last = pg;
        }
        const int r = rr(0, 999);
        const int k = r < 700 ? 0 : r < 930 ? 1 : r < 990 ? rr(2, 8) : r < 998 ? rr(10, 60) : rr(100, 400);
        for (int i = 0; i < k; i++)
        {
            const int reg = (int)(rnd() & 255);
            int val = (int)(rnd() & 255);
            if (reg == 0x01 && rr(0, 99) < 80)
                val &= 0x7F;
            const uint16_t a = (uint16_t)((rr(0, 99) < 3 ? 0x7700 : last) | reg);
            if (rr(0, 99) == 0)
                ev_write_drops(n, a, (uint8_t)val);
            else
                ev_write(n, a, (uint8_t)val);
        }
        if (n % 1000 == 999)
            ev_drops(n);
    }
}

/* Registers the engine takes, weighted toward the channels, their aliases
 * and BD, on any clock. */
static void ev_weighted(uint32_t seed, uint32_t samples)
{
    rs = seed * 2654435761u + 1;
    evs.clear();
    const uint16_t pg = 0x5600;
    ev_map(0, pg);
    ev_all(pg);
    static const uint8_t v01[10] = {0x00, 0x20, 0x08, 0x28, 0x80, 0xA0, 0x88, 0xA8, 0x25, 0xFF};
    static const uint8_t lat[6] = {0xAE, 0xAF, 0xBE, 0xBF, 0xCE, 0xCF};
    int bd = 0xC0;
    for (uint32_t n = 300; n < samples; n++)
    {
        const int r = rr(0, 999);
        const int k = r < 550 ? 0 : r < 900 ? 1 : r < 985 ? rr(2, 6) : r < 998 ? rr(10, 40) : rr(150, 300);
        for (int i = 0; i < k; i++)
        {
            const int c = rr(0, 8), off = modoff(c) + (rr(0, 1) ? 3 : 0);
            const int ch = rr(0, 3) ? c : c < 3 ? 9 + c : c;
            const int q = rr(0, 99);
            int reg, val = rr(0, 255);
            if (q < 3)
                reg = 0x01, val = v01[rr(0, 9)];
            else if (q < 6)
                reg = 0x08;
            else if (q < 18)
            {
                bd = (bd & 0xE0) | rr(0, 31);
                if (rr(0, 99) < 30)
                    bd ^= 0x20;
                if (rr(0, 99) < 15)
                    bd ^= 0xC0 & (int)rnd();
                reg = 0xBD, val = bd;
            }
            else if (q < 30)
                reg = 0xB0 + ch;
            else if (q < 40)
                reg = 0xA0 + ch;
            else if (q < 47)
                reg = 0xC0 + ch;
            else if (q < 50)
                reg = lat[rr(0, 5)];
            else
            {
                static const int grp[5] = {0x20, 0x40, 0x60, 0x80, 0xE0};
                reg = grp[(q - 50) / 10] + off;
            }
            ev_write(n, (uint16_t)(pg | reg), (uint8_t)val);
        }
        if (n % 1000 == 999)
            ev_drops(n);
    }
}

UTEST(opl, storm)
{
    ev_storm(1, 30000, false);
    ASSERT_OK(compare(30000, TICKS));
}
UTEST(opl, storm_maps)
{
    ev_storm(2, 30000, true);
    ASSERT_OK(compare(30000, TICKS));
}
UTEST(opl, storm_weighted)
{
    ev_weighted(3, 30000);
    ASSERT_OK(compare(30000, TICKS));
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vopl;
    dut->clk = 0;
    dut->xaddr_we = 0;
    dut->q_we = 0;
    dut->drops_take = 0;
    dut->eval();
    opl_reset();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
