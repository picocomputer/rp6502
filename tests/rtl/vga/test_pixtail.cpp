/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vpixtail.h"
#include "verilated.h"

#include "utest.h"
#include "vid_palette_tables.h"

#include <cstdint>
#include <cstring>
#include <vector>

UTEST_STATE();

static Vpixtail *dut;

static uint32_t g_rng = 0x12345678;
static uint32_t rnd()
{
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 17;
    g_rng ^= g_rng << 5;
    return g_rng;
}

static uint8_t xram[0x10000];
static uint32_t xram32(uint16_t word_addr)
{
    size_t at = (size_t)word_addr * 4;
    return (uint32_t)xram[at] | ((uint32_t)xram[at + 1] << 8)
        | ((uint32_t)xram[at + 2] << 16) | ((uint32_t)xram[at + 3] << 24);
}

struct seg
{
    bool imm;
    uint32_t bits;
    uint8_t ibits;
    uint16_t fg, bg;
    int px;
};

static uint16_t pal_entry(uint16_t ptr, bool pal_xram, int bpp_log,
                          uint8_t idx)
{
    if (!pal_xram)
        return bpp_log == 0 ? VID_COLOR_2[idx & 1] : VID_COLOR_256[idx];
    size_t ha = (size_t)(ptr >> 1) + idx;
    return (uint16_t)(xram[ha * 2] | (xram[ha * 2 + 1] << 8));
}

static void golden(const std::vector<seg> &segs, int bpp_log, bool rev,
                   uint16_t pal_ptr, bool pal_xram, int cw, uint16_t *out)
{
    int px = 0;
    for (const seg &s : segs)
    {
        if (s.imm)
        {
            for (int k = 0; k < s.px && px < cw; k++)
                out[px++] = (s.ibits >> (7 - (k & 7))) & 1 ? s.fg : s.bg;
            continue;
        }
        for (int k = 0; k < s.px && px < cw; k++)
        {
            uint32_t bp = s.bits + (uint32_t)k * (1u << bpp_log);
            uint32_t word = xram32((uint16_t)(bp >> 5));
            int biw = bp & 31;
            uint8_t byte = (uint8_t)(word >> ((biw >> 3) * 8));
            uint8_t idx = 0;
            switch (bpp_log)
            {
            case 0:
                idx = rev ? (byte >> (biw & 7)) & 1
                          : (byte >> (7 - (biw & 7))) & 1;
                break;
            case 1:
                idx = rev ? (byte >> ((biw >> 1 & 3) * 2)) & 3
                          : (byte >> ((3 - (biw >> 1 & 3)) * 2)) & 3;
                break;
            case 2:
                idx = rev ? (byte >> ((biw >> 2 & 1) * 4)) & 15
                          : (byte >> ((1 - (biw >> 2 & 1)) * 4)) & 15;
                break;
            default:
                idx = byte;
                break;
            }
            if (bpp_log == 4)
                out[px++] = (uint16_t)(word >> ((biw >> 4) * 16));
            else
                out[px++] = pal_entry(pal_ptr, pal_xram, bpp_log, idx);
        }
    }
}

struct capture
{
    uint16_t data[640];
    bool wrote[640];
    int n;
    bool done;
    int pal_loads;
};

static int g_feed_delay = 0;

static void run_line(const std::vector<seg> &segs, int bpp_log, bool rev,
                     uint16_t pal_ptr, bool pal_xram, int cw, capture *c)
{
    memset(c, 0, sizeof *c);
    int feed_hold = 0;

    static uint16_t palram[256];
    memset(palram, 0xEE, sizeof palram);

    size_t si = 0;
    bool gnt_q = false, gnt_q2 = false;
    uint16_t gnt_addr = 0, gnt_addr_q = 0, gnt_addr_q2 = 0;
    int gap = 0;

    dut->start = 1;
    dut->abort_i = 0;
    dut->cw = cw;
    dut->pal_ptr = pal_ptr;
    dut->pal_xram = pal_xram;
    dut->bpp_log = bpp_log;
    dut->reversed = rev;
    dut->seg_valid = 0;
    dut->a_gnt = 0;
    dut->clk = 1; dut->eval(); dut->clk = 0; dut->eval();
    dut->start = 0;

    for (long t = 0; t < 20000 && !c->done; t++)
    {
        if (si < segs.size() && feed_hold == 0)
        {
            const seg &s = segs[si];
            dut->seg_valid = 1;
            dut->seg_imm = s.imm;
            dut->seg_bits = s.bits;
            dut->seg_ibits = s.ibits;
            dut->seg_fg = s.fg;
            dut->seg_bg = s.bg;
            dut->seg_px = s.px;
        }
        else
            dut->seg_valid = 0;

        /* Read data arrives two clocks after its grant, as it does from
         * XRAM's render port in the machine. */
        dut->a_rdy = gnt_q2;
        if (gnt_q2)
            dut->a_rdata = xram32(gnt_addr_q2);

        dut->eval();

        bool gnt_now = false;
        if (dut->pixtail_a_req && gap == 0)
        {
            gnt_now = true;
            gnt_addr = dut->pixtail_a_addr;
            gap = rnd() % 4;
        }
        else if (gap > 0)
            gap--;
        dut->a_gnt = gnt_now;

        dut->eval();

        if (getenv("PIXTAIL_TRACE") && t < 400)
        {
            if (gnt_now)
                fprintf(stderr, "t%ld GNT %04X\n", t,
                        dut->pixtail_a_addr);
            if (dut->pixtail_seg_take)
                fprintf(stderr, "t%ld TAKE seg%zu\n", t, si);
        }

        if (dut->pixtail_pal_ld)
        {
            uint16_t w = dut->pixtail_pal_w;
            uint16_t words = dut->pixtail_pal_words;
            bool half = (pal_ptr & 2) != 0;
            uint32_t rd = dut->a_rdata;
            bool we_e = !half || w != words;
            bool we_o = !half || w != 0;
            uint16_t wa_o = half ? (uint16_t)(w - 1) : w;
            uint16_t wd_e = half ? (uint16_t)(rd >> 16) : (uint16_t)rd;
            uint16_t wd_o = half ? (uint16_t)rd : (uint16_t)(rd >> 16);
            if (we_e)
                palram[(w & 127) * 2] = wd_e;
            if (we_o)
                palram[(wa_o & 127) * 2 + 1] = wd_o;
            c->pal_loads++;
        }
        /* Both palette ports answer in the same clock, as palram does. */
        auto look = [&](uint8_t idx) -> uint16_t {
            if (pal_xram)
                return palram[idx];
            if (bpp_log == 0)
                return VID_COLOR_2[idx & 1];
            return VID_COLOR_256[idx];
        };
        dut->pal_q = look(dut->pixtail_pal_idx);
        dut->pal_q1 = look(dut->pixtail_pal_idx1);
        dut->eval();

        if (dut->pixtail_seg_take && si < segs.size())
        {
            si++;
            feed_hold = g_feed_delay;
        }
        else if (feed_hold > 0)
            feed_hold--;

        /* The pair: lane 0 at px_addr, lane 1 at px_addr + 1. */
        for (int lane = 0; lane < 2; lane++)
        {
            if (!(dut->pixtail_px_we & (1 << lane)))
                continue;
            int at = dut->pixtail_px_addr + lane;
            uint16_t px = (uint16_t)(dut->pixtail_px_data >> (16 * lane));
            if (getenv("PIXTAIL_TRACE") && t < 400)
                fprintf(stderr, "t%ld PX %d = %04X\n", t, at, px);
            if (at < 640 && !c->wrote[at])
            {
                c->wrote[at] = true;
                c->data[at] = px;
                c->n++;
            }
        }
        if (dut->pixtail_done)
            c->done = true;

        dut->clk = 1; dut->eval();
        gnt_q2 = gnt_q;
        gnt_addr_q2 = gnt_addr_q;
        gnt_q = gnt_now;
        gnt_addr_q = gnt_addr;
        dut->clk = 0; dut->eval();
    }
}

/* pixtail has no reset input, so each test builds a new model to start
 * from power-on state. */
static void fresh()
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vpixtail;
    dut->clk = 0;
    dut->eval();
    for (int i = 0; i < 4; i++)
    {
        dut->clk = 1; dut->eval();
        dut->clk = 0; dut->eval();
    }
}

static void fill_xram()
{
    for (size_t i = 0; i < sizeof xram; i++)
        xram[i] = (uint8_t)(rnd() >> 11);
}

static void check_line(int *utest_result, const std::vector<seg> &segs,
                       int bpp_log, bool rev, uint16_t pal_ptr,
                       bool pal_xram, int cw)
{
    static uint16_t want[640];
    golden(segs, bpp_log, rev, pal_ptr, pal_xram, cw, want);
    capture c;
    run_line(segs, bpp_log, rev, pal_ptr, pal_xram, cw, &c);
    ASSERT_TRUE(c.done);
    ASSERT_EQ(c.n, cw);
    int diffs = 0;
    for (int i = 0; i < cw; i++)
        if (c.data[i] != want[i])
        {
            if (diffs < 8)
                fprintf(stderr,
                        "px %d: rtl %04X want %04X (bpp %d rev %d)\n",
                        i, c.data[i], want[i], bpp_log, rev);
            diffs++;
        }
    ASSERT_EQ(diffs, 0);
}

UTEST(pixtail, one_xram_segment_every_depth_and_order)
{
    fresh();
    fill_xram();
    for (int bpp = 0; bpp <= 4; bpp++)
        for (int rev = 0; rev < (bpp <= 2 ? 2 : 1); rev++)
            for (int align = 0; align < 2; align++)
            {
                uint16_t pp = (uint16_t)(0x8000 + (align ? 2 : 0));
                std::vector<seg> s = {
                    {false, (0x1000u * 8) + (uint32_t)(bpp * 64), 0, 0, 0,
                     640}};
                check_line(utest_result, s, bpp, rev, pp, true, 640);
            }
}

UTEST(pixtail, builtin_palettes_and_raw16)
{
    fresh();
    fill_xram();
    std::vector<seg> s = {{false, 0x2000u * 8, 0, 0, 0, 640}};
    check_line(utest_result, s, 0, false, 0, false, 640);
    check_line(utest_result, s, 3, false, 0, false, 640);
    check_line(utest_result, s, 4, false, 0, false, 640);
}

UTEST(pixtail, wrap_split_segments)
{
    fresh();
    fill_xram();
    std::vector<seg> s;
    uint32_t base = 0x3001u * 8 + 4;
    int left = 640;
    while (left > 0)
    {
        int run = left < 100 ? left : 100;
        s.push_back({false, base, 0, 0, 0, run});
        left -= run;
    }
    check_line(utest_result, s, 1, false, 0x9000, true, 640);
    check_line(utest_result, s, 2, true, 0x9000, true, 640);
}

UTEST(pixtail, tile_shaped_segments)
{
    fresh();
    fill_xram();
    std::vector<seg> s;
    for (int i = 0; i < 80; i++)
        s.push_back({false,
                     (uint32_t)((0x4000 + (i * 37 % 512) * 16) * 8), 0, 0,
                     0, 8});
    check_line(utest_result, s, 2, false, 0xA000, true, 640);
    std::vector<seg> t;
    for (int i = 0; i < 128; i++)
        t.push_back({false,
                     (uint32_t)((0x5000 + (i * 53 % 512) * 16) * 8), 0, 0,
                     0, 5});
    check_line(utest_result, t, 3, false, 0xA200, true, 640);
}

UTEST(pixtail, immediate_cells_and_padding)
{
    fresh();
    fill_xram();
    std::vector<seg> s;
    for (int i = 0; i < 80; i++)
        s.push_back({true, 0, (uint8_t)rnd(), (uint16_t)rnd(),
                     (uint16_t)rnd(), 8});
    check_line(utest_result, s, 0, false, 0, false, 640);

    std::vector<seg> m = {
        {true, 0, 0, 0, 0, 100},
        {false, 0x6000u * 8, 0, 0, 0, 400},
        {true, 0, 0, 0, 0, 140},
    };
    check_line(utest_result, m, 3, false, 0xB000, true, 640);
}

UTEST(pixtail, address_stamped_short_segments)
{
    fresh();
    for (uint32_t ha = 0; ha < 0x8000; ha++)
    {
        xram[ha * 2] = (uint8_t)ha;
        xram[ha * 2 + 1] = (uint8_t)(ha >> 8);
    }
    std::vector<seg> s;
    for (int i = 0; i < 128; i++)
        s.push_back({false, (uint32_t)((0x1000 + i * 61 % 4096) * 32), 0,
                     0, 0, 5});
    check_line(utest_result, s, 4, false, 0, false, 640);
}

UTEST(pixtail, starved_feeder_alignment_sweep)
{
    fresh();
    fill_xram();
    for (int d = 1; d <= 12; d++)
    {
        g_feed_delay = d;
        std::vector<seg> s;
        for (int i = 0; i < 80; i++)
            s.push_back({true, 0, (uint8_t)rnd(), (uint16_t)rnd(),
                         (uint16_t)rnd(), 8});
        check_line(utest_result, s, 0, false, 0, false, 640);
        std::vector<seg> t;
        for (int i = 0; i < 80; i++)
            t.push_back({false,
                         (uint32_t)((0x4800 + (i * 41 % 512) * 16) * 8), 0,
                         0, 0, 8});
        check_line(utest_result, t, 2, false, 0xA400, true, 640);
    }
    g_feed_delay = 0;
}

UTEST(pixtail, mixed_and_narrow_and_reused)
{
    fresh();
    fill_xram();
    std::vector<seg> s = {
        {true, 0, 0xA5, 0x1234, 0x5678, 8},
        {false, 0x7000u * 8, 0, 0, 0, 150},
        {true, 0, 0x3C, 0xFFFF, 0x0000, 8},
        {false, 0x7100u * 8 + 12, 0, 0, 0, 154},
    };
    check_line(utest_result, s, 1, false, 0xC000, true, 320);
    check_line(utest_result, s, 1, false, 0xC000, true, 320);
}

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vpixtail;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
