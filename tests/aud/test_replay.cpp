/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * What reaches an audio engine, and from which writer.
 *
 * The engines learn only from writes on the snoop bus, and a chip's
 * register file does not survive a reconfigure, so a restore has to put
 * a page's bytes back on that bus somehow. rp6502.sv says the RISC-V's
 * writes are on it -- "the same mux the PSG watches, not the 6502's
 * engine alone" -- and everything the restore does rests on that claim
 * being true. Nothing tested it.
 *
 * So test it, and nothing else: every byte here comes out of a register
 * the program built for itself, never out of XRAM. That keeps the
 * question to "can this writer be heard" and away from "can this writer
 * read", which is a different question with a different answer.
 *
 * The measurement is sound, because silence is the failure that matters
 * and no register comparison catches it. A note asked for is either
 * heard or it is not.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>
#include <cstdlib>
#include <vector>

static Vrp6502 *dut;

/* Registers, by their ABI names, so the programs below read like the
 * assembly they are. */
enum { A1 = 11, A2 = 12, A3 = 13, A4 = 14, A5 = 15 };

static uint32_t i_lui(int rd, uint32_t imm20)
{
    return (imm20 << 12) | ((uint32_t)rd << 7) | 0x37u;
}
static uint32_t i_addi(int rd, int rs1, int imm)
{
    return (((uint32_t)imm & 0xFFFu) << 20) | ((uint32_t)rs1 << 15)
           | ((uint32_t)rd << 7) | 0x13u;
}
static uint32_t i_sw(int rs2, int rs1, int imm)
{
    return ((((uint32_t)imm >> 5) & 0x7Fu) << 25) | ((uint32_t)rs2 << 20)
           | ((uint32_t)rs1 << 15) | (2u << 12)
           | (((uint32_t)imm & 0x1Fu) << 7) | 0x23u;
}
static uint32_t i_sb(int rs2, int rs1, int imm)
{
    return ((((uint32_t)imm >> 5) & 0x7Fu) << 25) | ((uint32_t)rs2 << 20)
           | ((uint32_t)rs1 << 15) | (0u << 12)
           | (((uint32_t)imm & 0x1Fu) << 7) | 0x23u;
}
static uint32_t i_bne(int rs1, int rs2, int off)
{
    uint32_t u = (uint32_t)off;
    return (((u >> 12) & 1u) << 31) | (((u >> 5) & 0x3Fu) << 25)
           | ((uint32_t)rs2 << 20) | ((uint32_t)rs1 << 15) | (1u << 12)
           | (((u >> 1) & 0xFu) << 8) | (((u >> 11) & 1u) << 7) | 0x63u;
}
/* bne rs1, x0, -4: the one-instruction-back branch a delay loop needs. */
static uint32_t i_bnez_back(int rs1)
{
    return i_bne(rs1, 0, -4);
}
static uint32_t i_lbu(int rd, int rs1, int imm)
{
    return (((uint32_t)imm & 0xFFFu) << 20) | ((uint32_t)rs1 << 15)
           | (4u << 12) | ((uint32_t)rd << 7) | 0x03u;
}
#define I_NOP 0x00000013u
#define I_SPIN 0x0000006fu

/* 0xFE00 and 0x3000FE00 both want a low half no addi can carry alone,
 * so both are a lui one above and a subtraction back down. */
static void li32(std::vector<uint32_t> &p, int rd, uint32_t v)
{
    uint32_t hi = (v + 0x800u) >> 12;
    int lo = (int)(v - (hi << 12));
    p.push_back(i_lui(rd, hi & 0xFFFFFu));
    if (lo)
        p.push_back(i_addi(rd, rd, lo));
}

static void delay(std::vector<uint32_t> &p, int iters)
{
    p.push_back(i_addi(A3, 0, iters));
    p.push_back(i_addi(A3, A3, -1));
    p.push_back(i_bnez_back(A3));
}

static void tcm_poke(uint32_t byte_addr, uint32_t word)
{
    uint32_t w = byte_addr >> 2;
    dut->rootp->rp6502__DOT__rv__DOT__tcm0[w] = word & 0xff;
    dut->rootp->rp6502__DOT__rv__DOT__tcm1[w] = (word >> 8) & 0xff;
    dut->rootp->rp6502__DOT__rv__DOT__tcm2[w] = (word >> 16) & 0xff;
    dut->rootp->rp6502__DOT__rv__DOT__tcm3[w] = (word >> 24) & 0xff;
}

static uint32_t dropped()
{
    return dut->rootp->rp6502__DOT__aud_opl__DOT__aud_opl_dropped;
}

/* Channel 0's modulator is slot 0 and its carrier slot 3 -- the voice
 * tests/aud/test_opl.cpp sounds, so a failure here is a failure to
 * deliver registers whose effect is already known. */
static const uint8_t VOICE[][2] = {
    {0x20, 0x01}, {0x23, 0x01}, {0x40, 0x10}, {0x43, 0x00},
    {0x60, 0xF0}, {0x63, 0xF0}, {0x80, 0x77}, {0x83, 0x77},
    {0xC0, 0x0E}, {0xA0, 0x98}, {0xB0, 0x31},
};

#define OPL_PAGE 0xFE00u
#define AUD_BASE 0x70000000u
#define OPL_XADDR_OFF 8

/* Claim the page, wait out the chip's reset walk, then write the voice
 * into the page one register at a time. `pad` is how many nops sit
 * between two register writes. */
static std::vector<uint32_t> voice_program(int pad)
{
    std::vector<uint32_t> p;
    li32(p, A5, AUD_BASE);
    li32(p, A4, OPL_PAGE);
    p.push_back(i_sw(A4, A5, OPL_XADDR_OFF));
    /* aud_opl holds its core in reset for 255 machine clocks after a
     * pointer write while it walks the register file clear. */
    delay(p, 400);
    li32(p, A2, 0x30000000u + OPL_PAGE);
    for (auto &rv : VOICE)
    {
        p.push_back(i_addi(A1, 0, rv[1]));
        p.push_back(i_sb(A1, A2, rv[0]));
        for (int i = 0; i < pad; i++)
            p.push_back(I_NOP);
    }
    p.push_back(I_SPIN);
    return p;
}

static void run(const std::vector<uint32_t> &prog)
{
    dut->rst_n = 0;
    tb_clock(dut);
    tb_clock(dut);
    for (size_t i = 0; i < prog.size(); i++)
        tcm_poke((uint32_t)i * 4, prog[i]);
    dut->rst_n = 1;
    /* The 6502 out of reset, because the RIA's RW engine arbitrates the
     * XRAM port these accesses go through and a held machine is not the
     * machine the firmware runs on. */
    dut->rootp->rp6502__DOT__resb = 1;
    /* Long enough for the claim, the reset walk and every register. */
    for (int i = 0; i < 40000; i++)
        tb_clock(dut);
}

/* Sum and peak of |sample| over a span. Silence is zero on both. */
static uint64_t last_peak;
static uint64_t energy(int samples)
{
    uint64_t sum = 0;
    int seen = 0;
    last_peak = 0;
    for (long i = 0; seen < samples && i < 40000000L; i++)
    {
        tb_clock(dut);
        if (dut->rp6502_aud_valid)
        {
            uint64_t d = (uint64_t)std::abs((int)(int16_t)dut->rp6502_aud_l);
            sum += d;
            if (d > last_peak)
                last_peak = d;
            seen++;
        }
    }
    return sum;
}

UTEST(replay, a_write_from_the_risc_v_reaches_the_chip)
{
    /* The claim the whole restore rests on. Every byte came from a
     * register, so this asks only whether this writer is on the bus the
     * engine watches. */
    run(voice_program(8));
    ASSERT_EQ(0u, dropped());
    energy(64);
    ASSERT_GT(energy(512), (uint64_t)512);
    fprintf(stderr, "  risc-v paced: peak %llu dropped %u\n",
            (unsigned long long)last_peak, dropped());
    ASSERT_GT(last_peak, (uint64_t)1000);
}

UTEST(replay, even_back_to_back_the_risc_v_cannot_outrun_the_sequencer)
{
    /* aud_opl.sv takes a write only while it is idle, spends four
     * clocks on each one and drops in silence anything that arrives
     * between, and its header rests that on the 6502 at 8 MHz being
     * 6.3 clocks apart. The RISC-V is the other writer and its rate is
     * nobody's stated assumption, so measure it: nothing between the
     * register writes at all, and still not one dropped.
     *
     * It is the load and the store either side of a byte that keep it
     * honest -- a register write is never one instruction. Worth a test
     * because the margin is invisible in the source of either side, and
     * a faster core or a wider bus would spend it. */
    run(voice_program(0));
    fprintf(stderr, "  risc-v back to back: dropped %u\n", dropped());
    ASSERT_EQ(0u, dropped());
}

/* Four bytes wide, so byte a is lane a&3 at index a>>2. */
static void xram_poke(uint32_t a, uint8_t v)
{
    auto *r = dut->rootp;
    switch (a & 3)
    {
    case 0: r->rp6502__DOT__xram__DOT__mem0[a >> 2] = v; break;
    case 1: r->rp6502__DOT__xram__DOT__mem1[a >> 2] = v; break;
    case 2: r->rp6502__DOT__xram__DOT__mem2[a >> 2] = v; break;
    default: r->rp6502__DOT__xram__DOT__mem3[a >> 2] = v; break;
    }
}
static uint8_t xram_peek(uint32_t a)
{
    auto *r = dut->rootp;
    switch (a & 3)
    {
    case 0: return r->rp6502__DOT__xram__DOT__mem0[a >> 2];
    case 1: return r->rp6502__DOT__xram__DOT__mem1[a >> 2];
    case 2: return r->rp6502__DOT__xram__DOT__mem2[a >> 2];
    default: return r->rp6502__DOT__xram__DOT__mem3[a >> 2];
    }
}

UTEST(replay, the_replay_loop_delivers_the_page_that_is_there)
{
    /* aud_replay, as the firmware writes it and at machine level: the
     * page is already in XRAM -- a restore put it there and this test's
     * bench does the same -- and every byte is read and written straight
     * back so the engine hears it.
     *
     * Two things have to hold and neither is about sleeping. The page
     * has to still be the page afterwards, because a replay that writes
     * something other than what it read has destroyed the thing it was
     * sent to deliver. And the voice in it has to sound.
     *
     * Neither holds. The window does not read: a load from it answers
     * zero whatever is in the array, while a store into it lands and is
     * heard -- the two tests above are that same writer being heard
     * perfectly. So the loop delivers 256 zeros and writes them over the
     * page on its way through.
     *
     * Reading XRAM is not something this core needs to do, and no other
     * line of firmware does it. The fault is that aud_replay was written
     * as though it could. Whatever replaces it has to get the page's
     * bytes onto the snoop bus without a load from that window. */
    std::vector<uint32_t> p;
    li32(p, A5, AUD_BASE);
    li32(p, A4, OPL_PAGE);
    p.push_back(i_sw(A4, A5, OPL_XADDR_OFF));
    delay(p, 400);
    li32(p, A2, 0x30000000u + OPL_PAGE);
    li32(p, A3, 256);
    /* lbu a1,0(a2); sb a1,0(a2); addi a2,a2,1; addi a3,a3,-1; bne a3,x0 */
    p.push_back(i_lbu(A1, A2, 0));
    p.push_back(i_sb(A1, A2, 0));
    p.push_back(i_addi(A2, A2, 1));
    p.push_back(i_addi(A3, A3, -1));
    p.push_back(i_bne(A3, 0, -16));
    p.push_back(I_SPIN);

    dut->rst_n = 0;
    tb_clock(dut);
    for (uint32_t i = 0; i < 256; i++)
        xram_poke(OPL_PAGE + i, 0);
    for (auto &rv : VOICE)
        xram_poke(OPL_PAGE + rv[0], rv[1]);
    run(p);

    /* The page survived being replayed. */
    for (auto &rv : VOICE)
        ASSERT_EQ((uint32_t)rv[1], (uint32_t)xram_peek(OPL_PAGE + rv[0]));

    /* And the chip heard it. */
    energy(64);
    ASSERT_GT(energy(512), (uint64_t)512);
    fprintf(stderr, "  replay loop: peak %llu\n",
            (unsigned long long)last_peak);
    ASSERT_GT(last_peak, (uint64_t)1000);
}

/* The PSG's block, the same shape: aud_psg_xreg installs the pointer
 * and then replays the block so an engine that hears only writes learns
 * what the program wrote before it asked. No sleep anywhere near this
 * one -- it is every program that claims the PSG.
 *
 * tests/aud/test_aud.cpp already runs this path and passes, and cannot
 * tell whether it works: psg_pre_prog pokes the gate byte a second time
 * from the 6502 after the replay, and the volume nibble runs backwards
 * -- zero is loudest, fifteen is silence -- so a block replayed as all
 * zeros still answers that gate with a loud note. Hence this, which
 * asks the only question that separates them: is the block still the
 * block. */
UTEST(replay, the_psg_replay_loop_delivers_the_page_that_is_there)
{
    static const uint8_t BLOCK[] = {0x28, 0x05, 0x80, 0x00, 0x00, 0x00, 0x01};
    const uint32_t page = 0x8000u;

    std::vector<uint32_t> p;
    li32(p, A5, AUD_BASE);
    li32(p, A4, page);
    p.push_back(i_sw(A4, A5, 0)); /* AUD_PSG_XADDR */
    delay(p, 400);
    /* The replay's gate bits only count while this is set. */
    p.push_back(i_addi(A1, 0, 1));
    p.push_back(i_sw(A1, A5, 4)); /* AUD_PSG_REPLAY = 1 */
    li32(p, A2, 0x30000000u + page);
    li32(p, A3, 64);
    p.push_back(i_lbu(A1, A2, 0));
    p.push_back(i_sb(A1, A2, 0));
    p.push_back(i_addi(A2, A2, 1));
    p.push_back(i_addi(A3, A3, -1));
    p.push_back(i_bne(A3, 0, -16));
    p.push_back(i_addi(A1, 0, 0));
    p.push_back(i_sw(A1, A5, 4)); /* AUD_PSG_REPLAY = 0 */
    p.push_back(I_SPIN);

    dut->rst_n = 0;
    tb_clock(dut);
    for (uint32_t i = 0; i < 64; i++)
        xram_poke(page + i, 0);
    for (uint32_t i = 0; i < sizeof BLOCK; i++)
        xram_poke(page + i, BLOCK[i]);
    run(p);

    for (uint32_t i = 0; i < sizeof BLOCK; i++)
        ASSERT_EQ((uint32_t)BLOCK[i], (uint32_t)xram_peek(page + i));
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->eval();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
