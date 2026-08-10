/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The savestate engine reading the machine out.
 *
 * The claim is narrow and checkable: with the machine stopped, every
 * word the engine hands back is the word that is actually in the
 * machine. So the bench runs the machine, asks for a savestate, waits
 * for it to stop, and then reads the blob a word at a time — comparing
 * each against the same memory read straight out of the model, which is
 * a completely different path to the same bytes.
 *
 * The blob is nearly three hundred kilobytes, so the regions are
 * sampled rather than walked end to end; the sampling includes the
 * first and last word of each, which is where an off-by-one in the
 * region map would land.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>
#include <vector>

/* Word counts, which must match sst_engine's own. */
#define W_HDR 16u
#define W_STATE 64u
#define W_REGS 256u
#define W_SRAM 16384u
#define W_XRAM 16384u
#define W_CELLS 15360u
#define W_TCM 24576u
#define W_END 4u

#define B_HDR 0u
#define B_STATE (B_HDR + W_HDR)
#define B_REGS (B_STATE + W_STATE)
#define B_SRAM (B_REGS + W_REGS)
#define B_XRAM (B_SRAM + W_SRAM)
#define B_CELLS (B_XRAM + W_XRAM)
#define B_TCM (B_CELLS + W_CELLS)
#define B_END (B_TCM + W_TCM)
#define W_TOTAL (B_END + W_END)

#define SST_MAGIC 0x52365353u
#define SST_END_MAGIC 0x52365345u

static Vrp6502 *dut;

static void clk(void) { tb_clock(dut); }

/* The same bytes, fetched a different way: straight out of the model's
 * arrays rather than through the engine and the machine's bus. */
static uint32_t tcm_word(uint32_t w)
{
    auto *r = dut->rootp;
    return (uint32_t)r->rp6502__DOT__rv__DOT__tcm0[w]
           | ((uint32_t)r->rp6502__DOT__rv__DOT__tcm1[w] << 8)
           | ((uint32_t)r->rp6502__DOT__rv__DOT__tcm2[w] << 16)
           | ((uint32_t)r->rp6502__DOT__rv__DOT__tcm3[w] << 24);
}

static void tcm_put(uint32_t w, uint32_t v)
{
    auto *r = dut->rootp;
    r->rp6502__DOT__rv__DOT__tcm0[w] = v & 0xFF;
    r->rp6502__DOT__rv__DOT__tcm1[w] = (v >> 8) & 0xFF;
    r->rp6502__DOT__rv__DOT__tcm2[w] = (v >> 16) & 0xFF;
    r->rp6502__DOT__rv__DOT__tcm3[w] = (v >> 24) & 0xFF;
}

/* XRAM is four byte lanes indexed by the low two address bits, so a
 * word is one entry from each. */
static uint32_t xram_word(uint32_t w)
{
    auto *r = dut->rootp;
    return ((uint32_t)r->rp6502__DOT__xram__DOT__mem0[w] << 24)
           | ((uint32_t)r->rp6502__DOT__xram__DOT__mem1[w] << 16)
           | ((uint32_t)r->rp6502__DOT__xram__DOT__mem2[w] << 8)
           | (uint32_t)r->rp6502__DOT__xram__DOT__mem3[w];
}

/* The counter from the resume test, which gives the machine something
 * to have been doing. */
static const std::vector<uint32_t> COUNTER = {
    0x40000313, 0x5A500393, 0xFE732E23, 0x00000293,
    0x00128293, 0x00532023, 0x00430313, 0xFF5FF06F,
};

static void power_on(void)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vrp6502;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->sst_freeze = 0;
    dut->sst_save = 0;
    dut->sst_rd_idx = 0;
    dut->sst_rd_t = 0;
    dut->sst_dbg_halt = 0;
    dut->sst_dbg_halt_on_reset = 0;
    dut->sst_dbg_resume = 0;
    dut->sst_dbg_instr = 0;
    dut->sst_dbg_instr_vld = 0;
    dut->sst_dbg_data0 = 0;
    dut->sst_st_we = 0;
    dut->sst_tcm_sel = 0;
    dut->sst_tcm_we = 0;
    dut->sst_phi2_we = 0;
    dut->eval();
    for (uint32_t i = 0; i < COUNTER.size(); i++)
        tcm_put(i, COUNTER[i]);
    clk();
    clk();
    dut->rst_n = 1;
}

/* Ask for a savestate and wait for the machine to come to rest. */
static bool begin_save(void)
{
    dut->sst_save = 1;
    dut->eval();
    for (int i = 0; i < 40000; i++)
    {
        clk();
        if (dut->rp6502_sst_ready)
            return true;
    }
    return false;
}

static bool blob_word(uint32_t idx, uint32_t *out)
{
    /* The index is data; the toggle beside it is what says it has
     * stopped moving, which is the only thing the engine acts on. */
    dut->sst_rd_idx = idx;
    dut->sst_rd_t = !dut->sst_rd_t;
    dut->eval();
    /* The answer to the last index is still standing while the engine
     * notices this one, so wait for it to go away first. */
    for (int i = 0; i < 20000 && dut->rp6502_sst_rvalid; i++)
        clk();
    for (int i = 0; i < 20000; i++)
    {
        clk();
        if (dut->rp6502_sst_rvalid)
        {
            *out = dut->rp6502_sst_rdata;
            return true;
        }
    }
    return false;
}

UTEST(sst, it_stops_the_machine_before_it_says_ready)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();
    ASSERT_FALSE((int)dut->rp6502_sst_ready);

    ASSERT_TRUE(begin_save());
    /* Both halves are stopped, or the blob would move underneath it. */
    ASSERT_TRUE((int)dut->rp6502_sst_frozen);
    ASSERT_TRUE((int)dut->rp6502_sst_dbg_halted);

    /* And they stay stopped while the blob is being read. */
    uint32_t before = tcm_word(255);
    for (int i = 0; i < 5000; i++)
        clk();
    ASSERT_EQ(before, tcm_word(255));
    ASSERT_TRUE((int)dut->rp6502_sst_ready);
}

UTEST(sst, the_header_and_trailer_frame_the_blob)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();
    ASSERT_TRUE(begin_save());

    uint32_t w = 0;
    ASSERT_TRUE(blob_word(B_HDR + 0, &w));
    ASSERT_EQ(SST_MAGIC, w);
    ASSERT_TRUE(blob_word(B_HDR + 1, &w));
    ASSERT_EQ(1u, w);
    ASSERT_TRUE(blob_word(B_HDR + 2, &w));
    ASSERT_EQ((uint32_t)(W_TOTAL * 4), w);

    ASSERT_TRUE(blob_word(B_END + 1, &w));
    ASSERT_EQ((uint32_t)W_TOTAL, w);
    ASSERT_TRUE(blob_word(B_END + 2, &w));
    ASSERT_EQ(SST_END_MAGIC, w);
}

UTEST(sst, the_soft_cpus_memory_comes_back_word_for_word)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();
    ASSERT_TRUE(begin_save());

    /* The program, the mark it left, the record it was writing, and the
     * far end of the memory. */
    const uint32_t at[] = {0, 1, 7, 255, 256, 257, 300, W_TCM - 1};
    for (uint32_t i = 0; i < sizeof at / sizeof at[0]; i++)
    {
        uint32_t w = 0;
        ASSERT_TRUE(blob_word(B_TCM + at[i], &w));
        ASSERT_EQ(tcm_word(at[i]), w);
    }
}

UTEST(sst, xram_comes_back_word_for_word)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();
    /* Something to find: the lanes are written straight so the read
     * back through the byte window has to assemble them the same way. */
    auto *r = dut->rootp;
    for (uint32_t i = 0; i < 8; i++)
    {
        r->rp6502__DOT__xram__DOT__mem0[i] = (uint8_t)(0x10 + i);
        r->rp6502__DOT__xram__DOT__mem1[i] = (uint8_t)(0x20 + i);
        r->rp6502__DOT__xram__DOT__mem2[i] = (uint8_t)(0x30 + i);
        r->rp6502__DOT__xram__DOT__mem3[i] = (uint8_t)(0x40 + i);
    }
    ASSERT_TRUE(begin_save());

    const uint32_t at[] = {0, 1, 7, 1000, W_XRAM - 1};
    for (uint32_t i = 0; i < sizeof at / sizeof at[0]; i++)
    {
        uint32_t w = 0;
        ASSERT_TRUE(blob_word(B_XRAM + at[i], &w));
        ASSERT_EQ(xram_word(at[i]), w);
    }
}

UTEST(sst, the_flops_are_in_there_too)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();
    ASSERT_TRUE(begin_save());

    /* Composed from the model's own flops rather than read back
     * through the state port, which the engine is holding. */
    auto *r = dut->rootp;
    uint32_t w = 0;
    ASSERT_TRUE(blob_word(B_STATE + 0, &w));
    ASSERT_EQ((uint32_t)r->rp6502__DOT__cpu__DOT__a
                  | ((uint32_t)r->rp6502__DOT__cpu__DOT__x << 8)
                  | ((uint32_t)r->rp6502__DOT__cpu__DOT__y << 16)
                  | ((uint32_t)r->rp6502__DOT__cpu__DOT__s << 24),
              w);
    ASSERT_TRUE(blob_word(B_STATE + 1, &w));
    ASSERT_EQ((uint32_t)r->rp6502__DOT__cpu__DOT__pc
                  | ((uint32_t)r->rp6502__DOT__cpu__DOT__p << 16)
                  | ((uint32_t)r->rp6502__DOT__cpu__DOT__ir << 24),
              w);
    ASSERT_TRUE(blob_word(B_STATE + 3, &w));
    ASSERT_EQ((uint32_t)r->rp6502__DOT__cpu__DOT__nmi_pip
                  | ((uint32_t)r->rp6502__DOT__cpu__DOT__irq_pip << 16),
              w);

    /* The VIA's timers, which nothing else can read at all. */
    ASSERT_TRUE(blob_word(B_STATE + 5 + 2, &w));
    ASSERT_EQ((uint32_t)r->rp6502__DOT__via__DOT__t1_latch
                  | ((uint32_t)r->rp6502__DOT__via__DOT__t1_counter << 16),
              w);
    ASSERT_TRUE(blob_word(B_STATE + 5 + 4, &w));
    ASSERT_EQ((uint32_t)r->rp6502__DOT__via__DOT__t2_pip
                  | ((uint32_t)r->rp6502__DOT__via__DOT__t1_pip << 16),
              w);
}

/* The soft CPU's registers do not appear in any memory: they come out
 * of the core through its debug port while it is halted, and the only
 * check available is that they are the values the program made. */
UTEST(sst, the_soft_cpus_registers_are_in_there)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();
    ASSERT_TRUE(begin_save());

    /* x6 is the store pointer, and it is one word past the last thing
     * the record shows. */
    uint32_t count = 0;
    for (uint32_t i = 0; i < W_TCM && tcm_word(256 + i); i++)
        count++;
    ASSERT_TRUE(count > 2);

    uint32_t x5 = 0, x6 = 0, dpc = 0;
    ASSERT_TRUE(blob_word(B_STATE + 12 + 5, &x5));
    ASSERT_TRUE(blob_word(B_STATE + 12 + 6, &x6));
    ASSERT_TRUE(blob_word(B_STATE + 12 + 0, &dpc));
    /* The count is incremented and then stored, so at the instant the
     * machine stopped x5 is either the last value written or the one
     * about to be. x6 is the store pointer and only moves after a
     * store, so it follows the record exactly. */
    ASSERT_TRUE(x5 == count || x5 == count + 1);
    ASSERT_EQ(1024u + count * 4u, x6);
    /* Stopped inside the loop, which is words four to seven. */
    ASSERT_TRUE(dpc >= 16 && dpc <= 28);
}

UTEST(sst, letting_go_lets_the_machine_run_again)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();
    ASSERT_TRUE(begin_save());
    uint32_t held = tcm_word(255 + 1);

    dut->sst_save = 0;
    dut->eval();
    for (int i = 0; i < 200; i++)
        clk();
    ASSERT_FALSE((int)dut->rp6502_sst_ready);
    ASSERT_FALSE((int)dut->rp6502_sst_frozen);

    for (int i = 0; i < 4000; i++)
        clk();
    /* It carried on from where it was rather than starting over. */
    ASSERT_EQ(held, tcm_word(256));
    uint32_t count = 0;
    for (uint32_t i = 0; i < W_TCM && tcm_word(256 + i); i++)
        count++;
    ASSERT_TRUE(count > 3);
    for (uint32_t i = 0; i < count; i++)
        ASSERT_EQ(i + 1, tcm_word(256 + i));
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
