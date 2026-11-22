/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * wiring.sv's savestate registers, driven by the soft CPU as the firmware
 * drives them. Machine A holds its 6502, reads the 6502's, the VIA's, the
 * PHI2 count's and the bus's words, and lets go. Machine B, a fresh one
 * with A's RAM, holds, jams those words in and lets go. From the clock each
 * lets go, the two must write the same bytes on the same clocks.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "tb_stage.h"
#include "utest.h"

#include <cstdint>
#include <vector>

static Vwiring *dut;

/* A clock with the staging store answering, where a program leaves the
 * words it read for the test. */
static void tick()
{
    static const std::vector<uint8_t> none;
    tb_stage_tick(dut, none);
    tb_clock(dut);
}

/* Enough RV32I for the programs here. */
enum
{
    X0 = 0,
    T0 = 5,
    T1 = 6,
    A0 = 10,
    A1 = 11,
    A2 = 12,
    A3 = 13,
    A4 = 14
};

struct prog_t
{
    std::vector<uint32_t> w;

    uint32_t here() const { return (uint32_t)w.size() * 4; }
    void i_type(int32_t imm, int rs1, int f3, int rd, int op)
    {
        w.push_back(((uint32_t)imm & 0xFFFu) << 20 | rs1 << 15 | f3 << 12
                    | rd << 7 | op);
    }
    void s_type(int32_t imm, int rs2, int rs1, int f3)
    {
        uint32_t u = (uint32_t)imm;
        w.push_back((u >> 5 & 0x7Fu) << 25 | rs2 << 20 | rs1 << 15
                    | f3 << 12 | (u & 0x1Fu) << 7 | 0x23u);
    }
    void li(int rd, uint32_t v)
    {
        uint32_t up = (v + 0x800u) >> 12;
        w.push_back(up << 12 | rd << 7 | 0x37u);
        i_type((int32_t)(v - (up << 12)), rd, 0, rd, 0x13);
    }
    void addi(int rd, int rs1, int32_t imm) { i_type(imm, rs1, 0, rd, 0x13); }
    void andi(int rd, int rs1, int32_t imm) { i_type(imm, rs1, 7, rd, 0x13); }
    void lw(int rd, int32_t off, int rs1) { i_type(off, rs1, 2, rd, 0x03); }
    void sw(int rs2, int32_t off, int rs1) { s_type(off, rs2, rs1, 2); }
    void sb(int rs2, int32_t off, int rs1) { s_type(off, rs2, rs1, 0); }
    /* A branch back to at, taken when rs1 and rs2 compare as f3 says. */
    void b_back(int f3, int rs1, int rs2, uint32_t at)
    {
        uint32_t u = at - here();
        w.push_back((u >> 12 & 1u) << 31 | (u >> 5 & 0x3Fu) << 25
                    | rs2 << 20 | rs1 << 15 | f3 << 12 | (u >> 1 & 0xFu) << 8
                    | (u >> 11 & 1u) << 7 | 0x63u);
    }
    void spin() { w.push_back(0x0000006Fu); }

    /* MACH_HOLD on, then wait for MACH_HELD. a0 holds the window. */
    void hold()
    {
        li(A0, 0x90000000u);
        li(T0, 1);
        sw(T0, 0x00, A0);
        uint32_t at = here();
        lw(T1, 0x00, A0);
        andi(T1, T1, 2);
        b_back(0, T1, X0, at);
    }
};

/* ST_SEL's group and word for each JAM_DATA word. */
static const int JAM_WORDS = 15;
static uint32_t st_pick(int k)
{
    if (k < 5)
        return 1u << 3 | (uint32_t)k;
    if (k < 12)
        return 2u << 3 | (uint32_t)(k - 5);
    if (k < 14)
        return (uint32_t)(k - 12);
    return 3u << 3;
}

/* A program leaves the words it read at the start of the staging store,
 * past the cache, where the test reads them. */
static const uint32_t OUT = 0x60000000u;
static uint32_t out_word(int k)
{
    return tb_stage_word({}, (uint32_t)k * 4);
}

/* The RAM block's table that B takes A's words from. */
static const uint32_t TABLE = 0x41000u;

/* Counts a 6502 loop that reads open bus, writes it and keeps a count,
 * with the vectors in the RIA's window. */
static void load_6502(Vwiring *d)
{
    static const uint8_t loop[] = {
        0xE6, 0x10,       /* inc $10    */
        0xAD, 0x00, 0xFF, /* lda $FF00  */
        0x85, 0x11,       /* sta $11    */
        0x69, 0x03,       /* adc #3     */
        0x8D, 0x00, 0xFF, /* sta $FF00  */
        0x4C, 0x00, 0x02, /* jmp $0200  */
    };
    auto *r = d->rootp;
    for (size_t i = 0; i < sizeof loop; i++)
        r->wiring__DOT__sram__DOT__mem[0x0200 + i] = loop[i];
    r->wiring__DOT__ria__DOT__regs[0x1C] = 0x00;
    r->wiring__DOT__ria__DOT__regs[0x1D] = 0x02;
}

static void power_on(const prog_t &p)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->rst_n = 0;
    dut->eval();
    tick();
    tick();
    tb_ram_clear();
    tb_stage_clear();
    for (size_t i = 0; i < p.w.size(); i++)
        tb_ram_put((uint32_t)i, p.w[i]);
    dut->rst_n = 1;
}

/* Runs until the hold has been and gone, then records the bytes the
 * 6502 keeps for the clocks after. */
static bool trace_after_release(std::vector<uint32_t> &out,
                                std::vector<uint8_t> *ram)
{
    auto *r = dut->rootp;
    bool was = false;
    for (long i = 0; i < 400000; i++)
    {
        tick();
        bool held = r->wiring__DOT__held;
        if (was && !held)
        {
            if (ram)
                ram->assign(&r->wiring__DOT__sram__DOT__mem[0],
                            &r->wiring__DOT__sram__DOT__mem[0] + 0x10000);
            for (int k = 0; k < 20000; k++)
            {
                tick();
                const auto &m = r->wiring__DOT__sram__DOT__mem;
                out.push_back((uint32_t)m[0x10] | (uint32_t)m[0x11] << 8
                              | (uint32_t)m[0xFF00] << 16);
            }
            return true;
        }
        was = held;
    }
    return false;
}

UTEST(hold, a_jammed_machine_runs_on_as_the_held_one_does)
{
    for (int delay = 300; delay < 312; delay++)
    {
        /* A runs a while, holds, reads every word and lets go. */
        prog_t pa;
        pa.li(T0, (uint32_t)delay * 7);
        uint32_t at = pa.here();
        pa.addi(T0, T0, -1);
        pa.b_back(1, T0, X0, at);
        pa.hold();
        pa.li(A1, OUT);
        for (int k = 0; k < JAM_WORDS; k++)
        {
            pa.li(T0, st_pick(k));
            pa.sw(T0, 0x04, A0);
            pa.lw(T1, 0x08, A0);
            pa.sw(T1, k * 4, A1);
        }
        pa.sw(X0, 0x00, A0);
        pa.spin();
        power_on(pa);
        load_6502(dut);
        dut->rootp->wiring__DOT__resb = 1;
        std::vector<uint32_t> ta, tb;
        std::vector<uint8_t> ram;
        ASSERT_TRUE(trace_after_release(ta, &ram));
        std::vector<uint32_t> words;
        for (int k = 0; k < JAM_WORDS; k++)
            words.push_back(out_word(k));
        /* The 6502 is out of reset with its enable to come. */
        ASSERT_EQ(1u, words[12] & 1u);
        ASSERT_NE(0u, words[1]);

        /* B holds a 6502 still in reset, takes A's words and lets go. */
        prog_t pb;
        pb.hold();
        pb.li(A2, 0x40000000u);
        pb.li(T0, 1);
        pb.sb(T0, 0, A2);
        pb.li(A1, TABLE);
        for (int k = 0; k < JAM_WORDS; k++)
        {
            pb.lw(T1, k * 4, A1);
            pb.sw(T1, 0x40 + k * 4, A0);
        }
        pb.sw(X0, 0x0C, A0);
        pb.sw(X0, 0x00, A0);
        pb.spin();
        power_on(pb);
        for (int k = 0; k < JAM_WORDS; k++)
            tb_ram_put(TABLE / 4 + (uint32_t)k, words[(size_t)k]);
        load_6502(dut);
        for (size_t i = 0; i < ram.size(); i++)
            dut->rootp->wiring__DOT__sram__DOT__mem[i] = ram[i];
        ASSERT_TRUE(trace_after_release(tb, nullptr));
        ASSERT_EQ(ta.size(), tb.size());
        for (size_t i = 0; i < ta.size(); i++)
            if (ta[i] != tb[i])
            {
                fprintf(stderr, "  delay %d: clock %zu: %06X, A had %06X\n",
                        delay, i, tb[i], ta[i]);
                ASSERT_EQ(ta[i], tb[i]);
            }
        /* Something happened to compare. */
        ASSERT_NE(ta.front(), ta.back());
    }
}

/* The registers a savestate puts back read back what they were written,
 * and AUD_OWN is taken only under the hold and goes with it. */
UTEST(hold, the_other_registers_read_back)
{
    prog_t p;
    p.li(A1, OUT);
    int k = 0;
    auto keep = [&](int rs) { p.sw(rs, (k++) * 4, A1); };
    /* VID_FRAME. */
    p.li(A0, 0x50010000u);
    p.li(T0, 0x12345);
    p.sw(T0, 0xA0, A0);
    p.lw(T1, 0xA0, A0);
    keep(T1);
    /* VID_XPROG(5, 1, 0..3). */
    static const uint32_t xprog[] = {0xDEADBEEFu, 0x0000ABCDu, 0x8007FFFFu,
                                     0xCAFEF00Du};
    p.li(A0, 0x50020000u);
    for (int w = 0; w < 4; w++)
    {
        p.li(T0, xprog[w]);
        p.sw(T0, (5 * 16 + 4 + w) * 4, A0);
    }
    for (int w = 0; w < 4; w++)
    {
        p.lw(T1, (5 * 16 + 4 + w) * 4, A0);
        keep(T1);
    }
    /* MTIME, the low half first. */
    p.li(A0, 0xF0000000u);
    p.li(T0, 0x11111111u);
    p.sw(T0, 0x10, A0);
    p.li(T0, 0x22);
    p.sw(T0, 0x14, A0);
    p.lw(T1, 0x14, A0);
    keep(T1);
    p.lw(T1, 0x10, A0);
    keep(T1);
    /* AUD_OWN, refused without the hold. */
    p.li(A0, 0x90000000u);
    p.li(T0, 1);
    p.sw(T0, 0x14, A0);
    p.lw(T1, 0x14, A0);
    keep(T1);
    /* The console queue's pointers, word 20, taken only with
     * REGS_RESTORE. */
    p.li(A2, 0x20000000u);
    p.li(T0, 0x45);
    p.sw(T0, 20 * 4, A2);
    p.lw(T1, 20 * 4, A2);
    keep(T1);
    p.hold();
    p.li(T0, 1);
    p.sw(T0, 0x14, A0);
    p.lw(T1, 0x14, A0);
    keep(T1);
    /* PCM's buffer, word 5 of the audio's state. */
    p.li(A3, 0x90002000u);
    p.li(T0, 0x5A5AA5A5u);
    p.sw(T0, 5 * 4, A3);
    p.lw(T1, 5 * 4, A3);
    keep(T1);
    p.li(T0, 1);
    p.sw(T0, 0x10, A0);
    p.li(T0, 0x67);
    p.sw(T0, 20 * 4, A2);
    p.lw(T1, 20 * 4, A2);
    keep(T1);
    p.sw(X0, 0x10, A0);
    /* The hold's end drops AUD_OWN. */
    p.sw(X0, 0x00, A0);
    p.lw(T1, 0x00, A0);
    p.lw(T1, 0x14, A0);
    keep(T1);
    p.spin();
    power_on(p);
    for (int i = 0; i < 40000; i++)
        tick();
    auto got = [](int i) { return out_word(i); };
    ASSERT_EQ(0x12345u, got(0));
    for (int w = 0; w < 4; w++)
        ASSERT_EQ(xprog[w], got(1 + w));
    ASSERT_EQ(0x22u, got(5));
    ASSERT_GE(got(6), 0x11111111u);
    ASSERT_LT(got(6), 0x11111111u + 1000u);
    ASSERT_EQ(0u, got(7));
    ASSERT_EQ(0u, got(8));
    ASSERT_EQ(1u, got(9));
    ASSERT_EQ(0x5A5AA5A5u, got(10));
    ASSERT_EQ(0x67u, got(11));
    ASSERT_EQ(0u, got(12));
}

/* mtime stands still under the hold, and UPTIME counts on. */
UTEST(hold, uptime_counts_through_the_hold)
{
    prog_t p;
    p.li(A1, OUT);
    p.hold();
    p.li(A2, 0xF0000000u);
    p.lw(T0, 0x10, A2);
    p.sw(T0, 0, A1);
    p.lw(T0, 0x20, A2);
    p.sw(T0, 4, A1);
    p.li(T1, 200);
    uint32_t at = p.here();
    p.addi(T1, T1, -1);
    p.b_back(1, T1, X0, at);
    p.lw(T0, 0x10, A2);
    p.sw(T0, 8, A1);
    p.lw(T0, 0x20, A2);
    p.sw(T0, 12, A1);
    p.sw(X0, 0x00, A0);
    p.spin();
    power_on(p);
    for (int i = 0; i < 40000; i++)
        tick();
    ASSERT_EQ(out_word(0), out_word(2));
    ASSERT_GT(out_word(3), out_word(1) + 10u);
}

/* The jam is refused unless the hold is in force. */
UTEST(hold, a_jam_without_the_hold_does_nothing)
{
    prog_t p;
    p.li(A0, 0x90000000u);
    p.li(T0, 0x55);
    p.sw(T0, 0x40 + 14 * 4, A0);
    p.sw(X0, 0x0C, A0);
    p.spin();
    power_on(p);
    for (int i = 0; i < 4000; i++)
        tick();
    ASSERT_EQ(0, (int)dut->rootp->wiring__DOT__bus__DOT__hold);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    int rc = utest_main(argc, argv);
    if (dut)
    {
        dut->final();
        delete dut;
    }
    return rc;
}
