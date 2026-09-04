/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Halting the soft CPU and starting it again, which is the whole of
 * what "resume, not boot" means for it.
 *
 * Hazard3 carries a debug port this build has never driven. Halt is a
 * trap: the pipeline flushes and dpc holds the first instruction that
 * had not retired, so releasing runs that one next. While halted the
 * core still takes whole instructions on that port, so the registers
 * come out and go back through a few csr moves and dmdata0 — no dret,
 * no vector, nothing that looks like starting over.
 *
 * The claim under test is the same one the 6502's freeze test makes:
 * run a program, halt it, take everything, throw the core away, put it
 * all into a core that has just come out of reset, release — and the
 * work that comes out is the work the uninterrupted core would have
 * done, with no repetition and no gap.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>
#include <vector>

/* What the debug port takes, and what it does with it. Only these five
 * shapes are ever injected; nothing that branches, and nothing that
 * touches memory. */
#define I_EBREAK 0x00100073u
#define CSR_DMDATA0 0xbffu
#define CSR_DPC 0x7b1u

/* csrw dmdata0, xN — the register lands on the port. */
static uint32_t i_reg_out(int n) { return 0xbff01073u | ((uint32_t)n << 15); }
/* csrr xN, dmdata0 — what the port carries lands in the register. */
static uint32_t i_reg_in(int n) { return 0xbff02073u | ((uint32_t)n << 7); }
/* csrr xN, csr — rs1 is x0, so the CSR is read and not written. */
static uint32_t i_csr_read(uint32_t csr, int n)
{
    return (csr << 20) | 0x00002073u | ((uint32_t)n << 7);
}
/* csrw csr, xN — rd is x0, so the CSR is written and not read. */
static uint32_t i_csr_write(uint32_t csr, int n)
{
    return (csr << 20) | 0x00001073u | ((uint32_t)n << 15);
}

static Vwiring *dut;
static uint32_t g_data0;

/* data0's write enable can stand for several clocks while the
 * instruction stalls, and it can land in any of them, so every clock
 * latches it rather than a chosen few. */
static void clk(void)
{
    tb_clock(dut);
    if (dut->wiring_sst_dbg_data0_wen)
        g_data0 = dut->wiring_sst_dbg_data0;
}

static uint32_t tcm_word(uint32_t w)
{
    auto *r = dut->rootp;
    return (uint32_t)r->wiring__DOT__soc__DOT__tcm0[w]
           | ((uint32_t)r->wiring__DOT__soc__DOT__tcm1[w] << 8)
           | ((uint32_t)r->wiring__DOT__soc__DOT__tcm2[w] << 16)
           | ((uint32_t)r->wiring__DOT__soc__DOT__tcm3[w] << 24);
}

static void tcm_put(uint32_t w, uint32_t v)
{
    auto *r = dut->rootp;
    r->wiring__DOT__soc__DOT__tcm0[w] = v & 0xFF;
    r->wiring__DOT__soc__DOT__tcm1[w] = (v >> 8) & 0xFF;
    r->wiring__DOT__soc__DOT__tcm2[w] = (v >> 16) & 0xFF;
    r->wiring__DOT__soc__DOT__tcm3[w] = (v >> 24) & 0xFF;
}

static void power_on(const std::vector<uint32_t> &prog)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->sst_dbg_halt = 0;
    dut->sst_dbg_halt_on_reset = 0;
    dut->sst_dbg_resume = 0;
    dut->sst_dbg_instr = 0;
    dut->sst_dbg_instr_vld = 0;
    dut->sst_dbg_data0 = 0;
    dut->eval();
    for (uint32_t i = 0; i < prog.size(); i++)
        tcm_put(i, prog[i]);
    tb_clock(dut);
    tb_clock(dut);
    dut->rst_n = 1;
}

/* Hold the request until the core says it stopped. */
static bool halt(void)
{
    dut->sst_dbg_halt = 1;
    for (int i = 0; i < 4000; i++)
    {
        clk();
        if (dut->wiring_sst_dbg_halted)
            return true;
    }
    return false;
}

/* Push one instruction. The ready line falling is what says the core
 * took it — one bench clock is a clk_sys period and the core runs on
 * clk_rv, so letting go the moment ready is seen drops every other
 * instruction on the floor. */
static bool inject(uint32_t instr)
{
    dut->sst_dbg_instr = instr;
    dut->sst_dbg_instr_vld = 1;
    dut->eval();
    int i = 0;
    for (; i < 800 && !dut->wiring_sst_dbg_instr_rdy; i++)
        clk();
    bool taken = false;
    for (; i < 800 && !taken; i++)
    {
        clk();
        if (!dut->wiring_sst_dbg_instr_rdy)
            taken = true;
    }
    dut->sst_dbg_instr_vld = 0;
    dut->eval();
    return taken;
}

/* An ebreak is the only thing that says the instructions before it have
 * retired; the ready line only says the queue took them. */
static bool settle(void)
{
    if (!inject(I_EBREAK))
        return false;
    for (int i = 0; i < 400; i++)
    {
        if (dut->wiring_sst_dbg_ebreak)
            return true;
        clk();
    }
    return false;
}

static bool read_gpr(int n, uint32_t *out)
{
    g_data0 = 0xDEADBEEF;
    if (!inject(i_reg_out(n)) || !settle())
        return false;
    *out = g_data0;
    return true;
}

static bool write_gpr(int n, uint32_t v)
{
    dut->sst_dbg_data0 = v;
    dut->eval();
    return inject(i_reg_in(n)) && settle();
}

static bool read_csr(uint32_t csr, int scratch, uint32_t *out)
{
    g_data0 = 0xDEADBEEF;
    if (!inject(i_csr_read(csr, scratch)) || !inject(i_reg_out(scratch))
        || !settle())
        return false;
    *out = g_data0;
    return true;
}

static bool write_csr(uint32_t csr, int scratch, uint32_t v)
{
    dut->sst_dbg_data0 = v;
    dut->eval();
    return inject(i_reg_in(scratch)) && inject(i_csr_write(csr, scratch))
           && settle();
}

static void resume(void)
{
    dut->sst_dbg_halt = 0;
    dut->sst_dbg_resume = 1;
    dut->eval();
    for (int i = 0; i < 64; i++)
        clk();
    dut->sst_dbg_resume = 0;
    dut->eval();
}

/* Counts up in x5 and writes each value to TCM, so the memory is a
 * record of exactly which iterations ran and in what order. The store
 * address walks, so a repeated or skipped iteration is visible rather
 * than being overwritten.
 *
 * The prologue leaves a mark, and the loop never touches it again. That
 * is what tells a resumed core from a restarted one: the record alone
 * cannot, because a core that starts over writes the same ascending
 * run a second time and the memory ends up looking identical. */
static const std::vector<uint32_t> COUNTER = {
    0x40000313, // li  t1, 1024      x6  store base, word 256
    0x5A500393, // li  t2, 0x5A5     x7  the mark
    0xFE732E23, // sw  t2, -4(t1)        word 255
    0x00000293, // li  t0, 0         x5
    0x00128293, // addi t0, t0, 1    <- loop
    0x00532023, // sw  t0, 0(t1)
    0x00430313, // addi t1, t1, 4
    0xFF5FF06F, // j   -12
};

#define MARK_WORD 255u
#define MARK 0x000005A5u
#define REC_BASE 256u
#define REC_MAX 512u

/* What the program has written so far. */
static std::vector<uint32_t> record(void)
{
    std::vector<uint32_t> v;
    for (uint32_t i = 0; i < REC_MAX; i++)
    {
        uint32_t w = tcm_word(REC_BASE + i);
        if (!w)
            break;
        v.push_back(w);
    }
    return v;
}

UTEST(resume, halting_stops_it_where_it_is)
{
    power_on(COUNTER);
    for (int i = 0; i < 3000; i++)
        tb_clock(dut);
    ASSERT_TRUE(halt());

    std::vector<uint32_t> at_halt = record();
    ASSERT_TRUE(at_halt.size() > 2);

    /* Halted means halted: nothing may retire while it is held. */
    for (int i = 0; i < 3000; i++)
        tb_clock(dut);
    ASSERT_EQ(at_halt.size(), record().size());
}

UTEST(resume, its_registers_come_out_and_go_back)
{
    power_on(COUNTER);
    for (int i = 0; i < 3000; i++)
        tb_clock(dut);
    ASSERT_TRUE(halt());

    uint32_t t0 = 0;
    ASSERT_TRUE(read_gpr(5, &t0));
    /* x5 is the count, and the program has run for a while. */
    ASSERT_TRUE(t0 > 0);
    ASSERT_EQ(t0, (uint32_t)record().size());

    ASSERT_TRUE(write_gpr(5, 0x12345678u));
    uint32_t back = 0;
    ASSERT_TRUE(read_gpr(5, &back));
    ASSERT_EQ(0x12345678u, back);

    /* dpc is the program counter, and it is inside the loop. */
    uint32_t dpc = 0;
    ASSERT_TRUE(read_csr(CSR_DPC, 5, &dpc));
    ASSERT_TRUE(dpc >= 8 && dpc <= 0x18);
}

UTEST(resume, a_new_core_carries_on_rather_than_starting_over)
{
    power_on(COUNTER);
    for (int i = 0; i < 4000; i++)
        tb_clock(dut);
    ASSERT_TRUE(halt());

    /* Take everything the core is. x0 is hardwired, so 1..31. */
    uint32_t gpr[32] = {0};
    for (int n = 1; n < 32; n++)
        ASSERT_TRUE(read_gpr(n, &gpr[n]));
    uint32_t dpc = 0;
    ASSERT_TRUE(read_csr(CSR_DPC, 31, &dpc));
    /* Reading dpc used x31, so take it again after. */
    ASSERT_TRUE(read_gpr(31, &gpr[31]));

    std::vector<uint32_t> before = record();
    ASSERT_TRUE(before.size() > 4);
    /* The prologue ran once, long ago. */
    ASSERT_EQ(MARK, tcm_word(MARK_WORD));

    /* A different core, held before it executes anything. */
    std::vector<uint32_t> image = COUNTER;
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->sst_dbg_halt = 0;
    dut->sst_dbg_resume = 0;
    dut->sst_dbg_instr = 0;
    dut->sst_dbg_instr_vld = 0;
    dut->sst_dbg_data0 = 0;
    dut->rst_n = 0;
    dut->sst_dbg_halt_on_reset = 1;
    dut->eval();
    for (uint32_t i = 0; i < image.size(); i++)
        tcm_put(i, image[i]);
    /* The memory it was working on comes back with it, except for the
     * mark: poisoned, so that a core which ran its prologue again would
     * put the real value back and be caught doing it. */
    for (uint32_t i = 0; i < before.size(); i++)
        tcm_put(REC_BASE + i, before[i]);
    tcm_put(MARK_WORD, 0xBADBAD00u);
    tb_clock(dut);
    tb_clock(dut);
    dut->rst_n = 1;
    for (int i = 0; i < 64; i++)
        tb_clock(dut);
    ASSERT_TRUE((int)dut->wiring_sst_dbg_halted);
    /* Held from the first cycle, so nothing of the reset image ran. */
    ASSERT_EQ(before.size(), record().size());

    dut->sst_dbg_halt_on_reset = 0;
    dut->sst_dbg_halt = 1;
    dut->eval();

    /* CSRs through a scratch, then dpc, then the registers last —
     * nothing may use a GPR after they are set. */
    ASSERT_TRUE(write_csr(CSR_DPC, 31, dpc));
    for (int n = 1; n < 32; n++)
        ASSERT_TRUE(write_gpr(n, gpr[n]));

    resume();
    for (int i = 0; i < 4000; i++)
        tb_clock(dut);

    /* It resumed. Had it started over, the prologue would have run and
     * written the mark back. */
    ASSERT_EQ(0xBADBAD00u, tcm_word(MARK_WORD));

    std::vector<uint32_t> after = record();
    ASSERT_TRUE(after.size() > before.size());
    /* Everything it had already done stands, in order... */
    for (size_t i = 0; i < before.size(); i++)
        ASSERT_EQ(before[i], after[i]);
    /* ...and what it did next continues the count rather than
     * repeating one or skipping one. */
    for (size_t i = 0; i < after.size(); i++)
        ASSERT_EQ((uint32_t)(i + 1), after[i]);
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
