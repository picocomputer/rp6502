/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Hazard3 enters a halt the way it enters a trap, so dpc holds the address
 * of the first instruction that has not retired, and a resume continues
 * from dpc.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>
#include <vector>

#define I_EBREAK 0x00100073u
#define CSR_DMDATA0 0xbffu
#define CSR_DPC 0x7b1u

/* csrw dmdata0, xN */
static uint32_t i_reg_out(int n) { return 0xbff01073u | ((uint32_t)n << 15); }
/* csrr xN, dmdata0 */
static uint32_t i_reg_in(int n) { return 0xbff02073u | ((uint32_t)n << 7); }
/* csrr xN, csr */
static uint32_t i_csr_read(uint32_t csr, int n)
{
    return (csr << 20) | 0x00002073u | ((uint32_t)n << 7);
}
/* csrw csr, xN */
static uint32_t i_csr_write(uint32_t csr, int n)
{
    return (csr << 20) | 0x00001073u | ((uint32_t)n << 15);
}

static Vwiring *dut;
static uint32_t g_data0;

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

/* The ready line falls once the instruction FIFO has accepted the
 * instruction. The core samples the handshake only on clk_rv edges, which
 * come on every second tb_clock call, so vld is held until ready falls. */
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

/* The ready line shows only that the FIFO accepted an instruction, not that
 * the instruction has retired. Hazard3 raises dbg_instr_caught_ebreak only
 * once every instruction ahead of the ebreak has retired, so settle injects
 * an ebreak and waits for wiring_sst_dbg_ebreak. */
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
    ASSERT_TRUE(t0 > 0);
    ASSERT_EQ(t0, (uint32_t)record().size());

    ASSERT_TRUE(write_gpr(5, 0x12345678u));
    uint32_t back = 0;
    ASSERT_TRUE(read_gpr(5, &back));
    ASSERT_EQ(0x12345678u, back);

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

    uint32_t gpr[32] = {0};
    for (int n = 1; n < 32; n++)
        ASSERT_TRUE(read_gpr(n, &gpr[n]));
    uint32_t dpc = 0;
    ASSERT_TRUE(read_csr(CSR_DPC, 31, &dpc));
    ASSERT_TRUE(read_gpr(31, &gpr[31]));

    std::vector<uint32_t> before = record();
    ASSERT_TRUE(before.size() > 4);
    ASSERT_EQ(MARK, tcm_word(MARK_WORD));

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
    /* The record is restored and the mark is overwritten. A core that
     * started over would write the same ascending record again, so only
     * the mark being written back distinguishes a restart from a resume. */
    for (uint32_t i = 0; i < before.size(); i++)
        tcm_put(REC_BASE + i, before[i]);
    tcm_put(MARK_WORD, 0xBADBAD00u);
    tb_clock(dut);
    tb_clock(dut);
    dut->rst_n = 1;
    for (int i = 0; i < 64; i++)
        tb_clock(dut);
    ASSERT_TRUE((int)dut->wiring_sst_dbg_halted);
    ASSERT_EQ(before.size(), record().size());

    dut->sst_dbg_halt_on_reset = 0;
    dut->sst_dbg_halt = 1;
    dut->eval();

    /* write_csr passes dpc through x31, so dpc is written before the
     * GPRs are restored. */
    ASSERT_TRUE(write_csr(CSR_DPC, 31, dpc));
    for (int n = 1; n < 32; n++)
        ASSERT_TRUE(write_gpr(n, gpr[n]));

    resume();
    for (int i = 0; i < 4000; i++)
        tb_clock(dut);

    ASSERT_EQ(0xBADBAD00u, tcm_word(MARK_WORD));

    std::vector<uint32_t> after = record();
    ASSERT_TRUE(after.size() > before.size());
    for (size_t i = 0; i < before.size(); i++)
        ASSERT_EQ(before[i], after[i]);
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
