/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "utest.h"

#include <cstdint>
#include <vector>

/* The blob layout below must match sst_engine.sv, where ST_W65C02 is named
 * ST_CPU and SST_VER is named SST_VERSION. The engine has no ST_MACH and
 * selects state words 0 to 3 with SEL_MACH. */
#define W_HDR 16u
#define W_STATE 64u
#define W_REGS 256u
#define W_SRAM 16384u
#define W_XRAM 16384u
#define W_CELLS 15360u
#define W_XPROG 8192u
#define W_TCM 24576u
#define W_END 4u

#define B_HDR 0u
#define B_STATE (B_HDR + W_HDR)
#define B_REGS (B_STATE + W_STATE)
#define ST_MACH 0u
#define ST_W65C02 4u
#define ST_VIA 9u
#define ST_RV 16u
#define B_SRAM (B_REGS + W_REGS)
#define B_XRAM (B_SRAM + W_SRAM)
#define B_CELLS (B_XRAM + W_XRAM)
#define B_XPROG (B_CELLS + W_CELLS)
#define B_TCM (B_XPROG + W_XPROG)
#define B_END (B_TCM + W_TCM)
#define W_TOTAL (B_END + W_END)

#define SST_MAGIC 0x52365353u
#define SST_VER 3u
#define SST_END_MAGIC 0x52365345u

static Vwiring *dut;

static std::vector<uint8_t> g_stage;

static void clk(void)
{
    if (dut->wiring_stage_pend)
    {
        uint32_t a = dut->wiring_stage_addr & ~1u;
        dut->stage_half = (uint16_t)((a < g_stage.size() ? g_stage[a] : 0)
            | ((a + 1 < g_stage.size() ? g_stage[a + 1] : 0) << 8));
    }
    dut->stage_stall = 0;
    dut->eval();
    tb_clock(dut);
}

/* STAGE_BLOB is A_STAGE_OFF in sst_engine.sv. The engine assembles each
 * staged word with the byte at the lowest address in the most significant
 * byte. */
#define STAGE_BLOB 0x03F00000u
static void stage_word(uint32_t idx, uint32_t v)
{
    uint32_t a = STAGE_BLOB + idx * 4;
    if (g_stage.size() < a + 4)
        g_stage.resize(a + 4, 0);
    g_stage[a] = (uint8_t)(v >> 24);
    g_stage[a + 1] = (uint8_t)(v >> 16);
    g_stage[a + 2] = (uint8_t)(v >> 8);
    g_stage[a + 3] = (uint8_t)v;
}

static void stage_seal(void)
{
    stage_word(B_HDR + 0, SST_MAGIC);
    stage_word(B_HDR + 1, SST_VER);
    stage_word(B_HDR + 2, W_TOTAL * 4);
    uint32_t sum = 0;
    for (uint32_t i = 0; i < B_END; i++)
    {
        uint32_t a = STAGE_BLOB + i * 4;
        uint32_t w = ((uint32_t)g_stage[a] << 24) | ((uint32_t)g_stage[a + 1] << 16)
                     | ((uint32_t)g_stage[a + 2] << 8) | (uint32_t)g_stage[a + 3];
        sum = ((sum << 1) | (sum >> 31)) + w;
    }
    stage_word(B_END + 0, sum);
    stage_word(B_END + 1, W_TOTAL);
    stage_word(B_END + 2, SST_END_MAGIC);
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

/* XRAM is four byte-lane arrays, and the low two bits of an address select
 * the lane. An XRAM word is stored in the blob with mem0 in the least
 * significant byte, but an SRAM word is stored with the byte at the lowest
 * address in the most significant byte. */
static uint32_t xram_word(uint32_t w)
{
    auto *r = dut->rootp;
    return ((uint32_t)r->wiring__DOT__xram__DOT__mem3[w] << 24)
           | ((uint32_t)r->wiring__DOT__xram__DOT__mem2[w] << 16)
           | ((uint32_t)r->wiring__DOT__xram__DOT__mem1[w] << 8)
           | (uint32_t)r->wiring__DOT__xram__DOT__mem0[w];
}

/* This is the COUNTER program from test_resume.cpp, where each word is
 * commented with its assembly. */
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
    dut = new Vwiring;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->rst_n = 0;
    dut->sst_save = 0;
    dut->sst_rd_idx = 0;
    dut->sst_rd_t = 0;
    dut->sst_dbg_halt = 0;
    dut->sst_dbg_halt_on_reset = 0;
    dut->sst_dbg_resume = 0;
    dut->sst_dbg_instr = 0;
    dut->sst_dbg_instr_vld = 0;
    dut->sst_dbg_data0 = 0;
    dut->sst_tcm_sel = 0;
    dut->sst_tcm_we = 0;
    dut->sst_load = 0;
    dut->stage_stall = 0;
    dut->stage_half = 0;
    dut->eval();
    for (uint32_t i = 0; i < COUNTER.size(); i++)
        tcm_put(i, COUNTER[i]);
    clk();
    clk();
    dut->rst_n = 1;
}

/* The loop bound is large because the engine sums every word of the blob
 * before wiring_sst_ready rises. */
static bool begin_save(void)
{
    dut->sst_save = 1;
    dut->eval();
    for (long i = 0; i < 20000000L; i++)
    {
        clk();
        if (dut->wiring_sst_ready)
            return true;
    }
    return false;
}

static bool blob_word(uint32_t idx, uint32_t *out)
{
    dut->sst_rd_idx = idx;
    dut->sst_rd_t = !dut->sst_rd_t;
    dut->eval();
    /* wiring_sst_rvalid stays high for the previous index until the engine
     * samples the new one, so the loop waits for it to fall before waiting
     * for it to rise. */
    for (int i = 0; i < 20000 && dut->wiring_sst_rvalid; i++)
        clk();
    for (int i = 0; i < 20000; i++)
    {
        clk();
        if (dut->wiring_sst_rvalid)
        {
            *out = dut->wiring_sst_rdata;
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
    ASSERT_FALSE((int)dut->wiring_sst_ready);

    ASSERT_TRUE(begin_save());
    ASSERT_TRUE((int)dut->wiring_sst_stop_req);
    ASSERT_TRUE((int)dut->wiring_sst_dbg_halted);

    uint32_t before = tcm_word(255);
    for (int i = 0; i < 5000; i++)
        clk();
    ASSERT_EQ(before, tcm_word(255));
    ASSERT_TRUE((int)dut->wiring_sst_ready);
}

UTEST(sst, the_header_and_trailer_frame_the_blob)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();
    ASSERT_TRUE(begin_save());

    {
        uint32_t sum = 0;
        for (uint32_t i = 0; i < B_END; i++)
        {
            uint32_t w = 0;
            ASSERT_TRUE(blob_word(i, &w));
            sum = ((sum << 1) | (sum >> 31)) + w;
        }
        uint32_t tr = 0;
        ASSERT_TRUE(blob_word(B_END + 0, &tr));
        ASSERT_EQ(sum, tr);
    }

    uint32_t w = 0;
    ASSERT_TRUE(blob_word(B_HDR + 0, &w));
    ASSERT_EQ(SST_MAGIC, w);
    ASSERT_TRUE(blob_word(B_HDR + 1, &w));
    ASSERT_EQ(SST_VER, w);
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

    /* Words 0 to 7 hold COUNTER, word 255 holds the value its prologue
     * stores, and the words from 256 hold the counts its loop stores. */
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
    auto *r = dut->rootp;
    for (uint32_t i = 0; i < 8; i++)
    {
        r->wiring__DOT__xram__DOT__mem0[i] = (uint8_t)(0x10 + i);
        r->wiring__DOT__xram__DOT__mem1[i] = (uint8_t)(0x20 + i);
        r->wiring__DOT__xram__DOT__mem2[i] = (uint8_t)(0x30 + i);
        r->wiring__DOT__xram__DOT__mem3[i] = (uint8_t)(0x40 + i);
    }
    ASSERT_TRUE(begin_save());

    const uint32_t at[] = {0, 1, 7, 1000, W_XRAM - 1};
    for (uint32_t i = 0; i < sizeof at / sizeof at[0]; i++)
    {
        uint32_t w = 0;
        ASSERT_TRUE(blob_word(B_XRAM + at[i], &w));
        if (xram_word(at[i]) != w)
            fprintf(stderr, "xram idx %u: model %08X blob %08X\n",
                    at[i], xram_word(at[i]), w);
        ASSERT_EQ(xram_word(at[i]), w);
    }
}

UTEST(sst, the_scanline_program_comes_back)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();
    auto *r = dut->rootp;
    for (uint32_t e = 0; e < 4; e++)
    {
        r->wiring__DOT__prog__DOT__fill_e[e] = 0x8001C000u + e;
        r->wiring__DOT__prog__DOT__fill_c[e] = (uint16_t)(0x4400u + e);
        r->wiring__DOT__prog__DOT__spr_e[e] = 0x80000u + e;
        r->wiring__DOT__prog__DOT__spr_c[e] = 0x77000000u + e;
    }
    ASSERT_TRUE(begin_save());

    for (uint32_t e = 0; e < 4; e++)
    {
        uint32_t w = 0;
        ASSERT_TRUE(blob_word(B_XPROG + e * 4 + 0, &w));
        ASSERT_EQ(0x8001C000u + e, w);
        ASSERT_TRUE(blob_word(B_XPROG + e * 4 + 1, &w));
        ASSERT_EQ(0x4400u + e, w);
        /* spr_e keeps bit 31 of the word in its bit 19, so 0x80000 in
         * the array reads back as 0x80000000. */
        ASSERT_TRUE(blob_word(B_XPROG + e * 4 + 2, &w));
        ASSERT_EQ(0x80000000u | e, w);
        ASSERT_TRUE(blob_word(B_XPROG + e * 4 + 3, &w));
        ASSERT_EQ(0x77000000u + e, w);
    }
}

UTEST(sst, the_flops_are_in_there_too)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();
    ASSERT_TRUE(begin_save());

    auto *r = dut->rootp;
    uint32_t w = 0;
    ASSERT_TRUE(blob_word(B_STATE + ST_W65C02 + 0, &w));
    ASSERT_EQ((uint32_t)r->wiring__DOT__cpu__DOT__a
                  | ((uint32_t)r->wiring__DOT__cpu__DOT__x << 8)
                  | ((uint32_t)r->wiring__DOT__cpu__DOT__y << 16)
                  | ((uint32_t)r->wiring__DOT__cpu__DOT__s << 24),
              w);
    ASSERT_TRUE(blob_word(B_STATE + ST_W65C02 + 1, &w));
    ASSERT_EQ((uint32_t)r->wiring__DOT__cpu__DOT__pc
                  | ((uint32_t)r->wiring__DOT__cpu__DOT__p << 16)
                  | ((uint32_t)r->wiring__DOT__cpu__DOT__ir << 24),
              w);
    ASSERT_TRUE(blob_word(B_STATE + ST_W65C02 + 3, &w));
    ASSERT_EQ((uint32_t)r->wiring__DOT__cpu__DOT__nmi_pip
                  | ((uint32_t)r->wiring__DOT__cpu__DOT__irq_pip << 16),
              w);

    ASSERT_TRUE(blob_word(B_STATE + ST_VIA + 2, &w));
    ASSERT_EQ((uint32_t)r->wiring__DOT__via__DOT__t1_latch
                  | ((uint32_t)r->wiring__DOT__via__DOT__t1_counter << 16),
              w);
    ASSERT_TRUE(blob_word(B_STATE + ST_VIA + 4, &w));
    ASSERT_EQ((uint32_t)r->wiring__DOT__via__DOT__t2_pip
                  | ((uint32_t)r->wiring__DOT__via__DOT__t1_pip << 16),
              w);
}

UTEST(sst, the_soft_cpus_registers_are_in_there)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();
    ASSERT_TRUE(begin_save());

    uint32_t count = 0;
    for (uint32_t i = 0; i < W_TCM && tcm_word(256 + i); i++)
        count++;
    ASSERT_TRUE(count > 2);

    uint32_t x5 = 0, x6 = 0, dpc = 0;
    ASSERT_TRUE(blob_word(B_STATE + ST_RV + 5, &x5));
    ASSERT_TRUE(blob_word(B_STATE + ST_RV + 6, &x6));
    ASSERT_TRUE(blob_word(B_STATE + ST_RV + 0, &dpc));
    /* COUNTER increments x5 before storing it, so a halt between the two
     * leaves x5 one above the last stored value. */
    ASSERT_TRUE(x5 == count || x5 == count + 1);
    ASSERT_EQ(1024u + count * 4u, x6);
    /* The loop is words 4 to 7, which are byte addresses 16 to 28. */
    ASSERT_TRUE(dpc >= 16 && dpc <= 28);
}

UTEST(sst, letting_go_lets_the_machine_run_again)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();
    ASSERT_TRUE(begin_save());
    uint32_t held = tcm_word(255 + 1);
    uint32_t count_before = 0;
    for (uint32_t i = 0; i < W_TCM && tcm_word(256 + i); i++)
        count_before++;

    dut->sst_save = 0;
    dut->eval();
    for (int i = 0; i < 200; i++)
        clk();
    ASSERT_FALSE((int)dut->wiring_sst_ready);
    ASSERT_FALSE((int)dut->wiring_sst_stop_req);

    for (int i = 0; i < 8000; i++)
        clk();
    ASSERT_EQ(held, tcm_word(256));
    uint32_t count = 0;
    for (uint32_t i = 0; i < W_TCM && tcm_word(256 + i); i++)
        count++;
    ASSERT_GT(count, count_before);
    for (uint32_t i = 0; i < count; i++)
        ASSERT_EQ(i + 1, tcm_word(256 + i));
}

UTEST(sst, a_load_puts_the_blob_back)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();

    g_stage.assign(STAGE_BLOB + (W_TOTAL + 4) * 4, 0);
    for (uint32_t i = 0; i < W_TOTAL; i++)
        stage_word(i, 0);
    for (uint32_t i = 0; i < 8; i++)
    {
        stage_word(B_SRAM + i, 0x5A5A0000u + i);
        stage_word(B_XRAM + i, 0xC0DE0000u + i);
        stage_word(B_CELLS + i, 0xCE110000u + i);
        stage_word(B_TCM + i, 0x7C700000u + i);
    }
    for (uint32_t e = 0; e < 4; e++)
    {
        stage_word(B_XPROG + e * 4 + 0, 0x80012000u + e);
        stage_word(B_XPROG + e * 4 + 1, 0x3300u + e);
        stage_word(B_XPROG + e * 4 + 2, 0x80000000u | (0x1000u + e));
        stage_word(B_XPROG + e * 4 + 3, 0x66000000u + e);
    }
    /* ST_MACH word 0 takes the 6502 out of reset and word 1 sets PHI2 to
     * 3000 kHz. The 6502 is staged at an opcode fetch from 0x0010, and
     * SRAM bytes 16 to 31 hold WAI, so after the jam it executes WAI and
     * its registers keep the jammed values. */
    stage_word(B_STATE + ST_MACH + 0, 1);
    stage_word(B_STATE + ST_MACH + 1, 3000);
    stage_word(B_STATE + ST_W65C02 + 0, 0x11223344u);
    stage_word(B_STATE + ST_W65C02 + 1, 0xA5B50010u);
    stage_word(B_STATE + ST_W65C02 + 4, 0x00100001u);
    stage_word(B_STATE + ST_VIA + 2, 0xBEEF1234u);
    for (uint32_t i = 4; i < 8; i++)
        stage_word(B_SRAM + i, 0xCBCBCBCBu);
    stage_seal();

    auto *r = dut->rootp;
    for (uint32_t i = 0; i < 8; i++)
    {
        r->wiring__DOT__xram__DOT__mem0[i] = 0xEE;
        r->wiring__DOT__xram__DOT__mem1[i] = 0xEE;
        r->wiring__DOT__xram__DOT__mem2[i] = 0xEE;
        r->wiring__DOT__xram__DOT__mem3[i] = 0xEE;
        r->wiring__DOT__mode0__DOT__cell0[i] = 0xEE;
        r->wiring__DOT__mode0__DOT__cell1[i] = 0xEE;
        r->wiring__DOT__mode0__DOT__cell2[i] = 0xEE;
        r->wiring__DOT__mode0__DOT__cell3[i] = 0xEE;
    }
    for (uint32_t i = 0; i < 32; i++)
        r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i] = 0xEE;

    dut->sst_load = 1;
    dut->eval();
    long guard = 0;
    while (!dut->wiring_sst_load_done && guard++ < 40000000L)
        clk();
    ASSERT_TRUE((int)dut->wiring_sst_load_done);

    for (uint32_t i = 0; i < 8; i++)
    {
        ASSERT_EQ(0xC0DE0000u + i, xram_word(i));
        ASSERT_EQ(0x7C700000u + i, tcm_word(i));
    }
    for (uint32_t i = 0; i < 4; i++)
    {
        ASSERT_EQ(0x5Au, r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i * 4]);
        ASSERT_EQ(0x5Au,
                  r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i * 4 + 1]);
        ASSERT_EQ(0x00u,
                  r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i * 4 + 2]);
        ASSERT_EQ(i, (uint32_t)r
                         ->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i * 4 + 3]);
    }
    for (uint32_t i = 16; i < 32; i++)
        ASSERT_EQ(0xCBu,
                  (uint32_t)r->wiring__DOT__g_ram_bram__DOT__sram__DOT__mem[i]);
    for (uint32_t i = 0; i < 8; i++)
    {
        ASSERT_EQ(i, (uint32_t)r->wiring__DOT__mode0__DOT__cell0[i]);
        ASSERT_EQ(0x00u, (uint32_t)r->wiring__DOT__mode0__DOT__cell1[i]);
        ASSERT_EQ(0x11u, (uint32_t)r->wiring__DOT__mode0__DOT__cell2[i]);
        ASSERT_EQ(0xCEu, (uint32_t)r->wiring__DOT__mode0__DOT__cell3[i]);
    }

    for (uint32_t e = 0; e < 4; e++)
    {
        ASSERT_EQ(0x80012000u + e,
                  (uint32_t)r->wiring__DOT__prog__DOT__fill_e[e]);
        ASSERT_EQ(0x3300u + e,
                  (uint32_t)r->wiring__DOT__prog__DOT__fill_c[e]);
        ASSERT_EQ(0x80000u | (0x1000u + e),
                  (uint32_t)r->wiring__DOT__prog__DOT__spr_e[e]);
        ASSERT_EQ(0x66000000u + e,
                  (uint32_t)r->wiring__DOT__prog__DOT__spr_c[e]);
    }

    /* The 6502 takes the jammed words in S_LD_JAM2, and the engine enters
     * S_LD_JAM only after sst_load falls, so A has not taken the jammed
     * value here. */
    ASSERT_NE(0x44u, (uint32_t)r->wiring__DOT__cpu__DOT__a);

    dut->sst_load = 0;
    dut->eval();
    for (int i = 0; i < 200; i++)
        clk();
    ASSERT_FALSE((int)dut->wiring_sst_stop_req);

    ASSERT_EQ(0x44u, (uint32_t)r->wiring__DOT__cpu__DOT__a);
    ASSERT_EQ(0x33u, (uint32_t)r->wiring__DOT__cpu__DOT__x);
    ASSERT_EQ(0x22u, (uint32_t)r->wiring__DOT__cpu__DOT__y);
    ASSERT_EQ(0x11u, (uint32_t)r->wiring__DOT__cpu__DOT__s);
    ASSERT_EQ(0xB5u, (uint32_t)r->wiring__DOT__cpu__DOT__p);
    ASSERT_EQ(0x1234u, (uint32_t)r->wiring__DOT__via__DOT__t1_latch);

    ASSERT_TRUE(begin_save());
    uint32_t w = 0;
    ASSERT_TRUE(blob_word(B_STATE + ST_MACH + 0, &w));
    ASSERT_EQ(1u, w);
    ASSERT_TRUE(blob_word(B_STATE + ST_MACH + 1, &w));
    ASSERT_EQ(3000u, w);
}

/* A dpc of 0x14 puts the soft CPU in front of `sw x5, 0(x6)` with x5 and
 * x6 set to MARK and PTR. A resumed core writes MARK at PTR and MARK + 1
 * in the next word. A core that started over would instead write words
 * 255 and 256, which the blob leaves at zero. */
UTEST(sst, the_soft_cpus_registers_go_back_in)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();

    g_stage.assign(STAGE_BLOB + (W_TOTAL + 4) * 4, 0);
    for (uint32_t i = 0; i < W_TOTAL; i++)
        stage_word(i, 0);
    for (uint32_t i = 0; i < COUNTER.size(); i++)
        stage_word(B_TCM + i, COUNTER[i]);

    const uint32_t MARK = 0x11110000u;
    const uint32_t PTR = 0x4000u;
    stage_word(B_STATE + ST_RV + 0, 0x14u);
    stage_word(B_STATE + ST_RV + 5, MARK);
    stage_word(B_STATE + ST_RV + 6, PTR);
    stage_word(B_STATE + ST_MACH + 0, 1);
    stage_seal();

    dut->sst_load = 1;
    dut->eval();
    long guard = 0;
    while (!dut->wiring_sst_load_done && guard++ < 40000000L)
        clk();
    ASSERT_TRUE((int)dut->wiring_sst_load_done);

    dut->sst_load = 0;
    dut->eval();
    for (int i = 0; i < 6000; i++)
        clk();

    ASSERT_EQ(MARK, tcm_word(PTR / 4));
    ASSERT_EQ(MARK + 1, tcm_word(PTR / 4 + 1));
    ASSERT_EQ(0u, tcm_word(255));
    ASSERT_EQ(0u, tcm_word(256));
}

static void load_and_wait(void)
{
    dut->sst_load = 1;
    dut->eval();
    long guard = 0;
    while (!dut->wiring_sst_load_done && guard++ < 40000000L)
        clk();
}

UTEST(sst, a_blob_that_does_not_add_up_is_refused)
{
    for (int pass = 0; pass < 2; pass++)
    {
        power_on();
        auto *r = dut->rootp;
        for (int i = 0; i < 3000; i++)
            clk();

        g_stage.assign(STAGE_BLOB + (W_TOTAL + 4) * 4, 0);
        for (uint32_t i = 0; i < W_TOTAL; i++)
            stage_word(i, 0);
        for (uint32_t i = 0; i < 8; i++)
            stage_word(B_XRAM + i, 0xC0DE0000u + i);
        stage_seal();
        if (pass == 0)
            stage_word(B_HDR + 0, 0x4E4F5045u);
        else
            stage_word(B_XRAM + 3, 0xDEADBEEFu);

        for (uint32_t i = 0; i < 8; i++)
        {
            r->wiring__DOT__xram__DOT__mem0[i] = 0xEE;
            r->wiring__DOT__xram__DOT__mem1[i] = 0xEE;
            r->wiring__DOT__xram__DOT__mem2[i] = 0xEE;
            r->wiring__DOT__xram__DOT__mem3[i] = 0xEE;
        }
        uint32_t held = tcm_word(256);

        load_and_wait();
        ASSERT_TRUE((int)dut->wiring_sst_load_done);
        ASSERT_TRUE((int)dut->wiring_sst_load_err);

        for (uint32_t i = 0; i < 8; i++)
            ASSERT_EQ(0xEEEEEEEEu, xram_word(i));

        dut->sst_load = 0;
        dut->eval();
        for (int i = 0; i < 4000; i++)
            clk();
        ASSERT_EQ(held, tcm_word(256));
        uint32_t count = 0;
        for (uint32_t i = 0; i < W_TCM && tcm_word(256 + i); i++)
            count++;
        ASSERT_TRUE(count > 3);
        for (uint32_t i = 0; i < count; i++)
            ASSERT_EQ(i + 1, tcm_word(256 + i));
    }
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

/* Reading regs word 16 takes a byte off the console queue, so the blob
 * holds the queue in regs words 20 to 24 instead: the count and the read
 * and write pointers in word 20 and the sixteen bytes in words 21 to 24. */
UTEST(sst, the_console_queue_survives)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();

    g_stage.assign(STAGE_BLOB + (W_TOTAL + 4) * 4, 0);
    for (uint32_t i = 0; i < W_TOTAL; i++)
        stage_word(i, 0);
    /* Word 20 describes a full queue, with a count of 16 and both pointers
     * at 5, and every byte is distinct, so a rotated or partial restore
     * reads back differently. */
    stage_word(B_REGS + 20, (16u << 8) | (5u << 4) | 5u);
    stage_word(B_REGS + 21, 0xD3C2B1A0u);
    stage_word(B_REGS + 22, 0xD7C6B5A4u);
    stage_word(B_REGS + 23, 0xDBCAB9A8u);
    stage_word(B_REGS + 24, 0xDFCEBDACu);
    /* ST_MACH word 0 is 0, so the 6502 stays in reset and pushes nothing
     * to the queue. */
    stage_word(B_STATE + ST_MACH + 0, 0);
    stage_seal();

    dut->sst_load = 1;
    dut->eval();
    long guard = 0;
    while (!dut->wiring_sst_load_done && guard++ < 40000000L)
        clk();
    ASSERT_TRUE((int)dut->wiring_sst_load_done);
    dut->sst_load = 0;
    dut->eval();
    for (int i = 0; i < 200; i++)
        clk();

    ASSERT_TRUE(begin_save());
    uint32_t w = 0;
    ASSERT_TRUE(blob_word(B_REGS + 20, &w));
    ASSERT_EQ((16u << 8) | (5u << 4) | 5u, w & 0x1FFFu);
    ASSERT_TRUE(blob_word(B_REGS + 21, &w));
    ASSERT_EQ(0xD3C2B1A0u, w);
    ASSERT_TRUE(blob_word(B_REGS + 22, &w));
    ASSERT_EQ(0xD7C6B5A4u, w);
    ASSERT_TRUE(blob_word(B_REGS + 23, &w));
    ASSERT_EQ(0xDBCAB9A8u, w);
    ASSERT_TRUE(blob_word(B_REGS + 24, &w));
    ASSERT_EQ(0xDFCEBDACu, w);
}
