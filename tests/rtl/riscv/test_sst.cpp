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

#include "Vwiring.h"
#include "Vwiring___024root.h"

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
#define W_XPROG 8192u
#define W_TCM 24576u
#define W_END 4u

#define B_HDR 0u
#define B_STATE (B_HDR + W_HDR)
#define B_REGS (B_STATE + W_STATE)
/* Inside the state page: the machine's own flops first, because whether
 * the 6502 is out of reset decides whether jamming its registers means
 * anything, then the 6502, the VIA, and the soft CPU's registers. */
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
/* sst_engine.sv's SST_VERSION. It moves whenever the state page or the
 * firmware's restore contract changes, and a blob the bench builds has
 * to carry the current one or the engine is right to refuse it. */
#define SST_VER 3u
#define SST_END_MAGIC 0x52365345u

static Vwiring *dut;

/* The staging store lives outside the machine, so the bench is it: a
 * byte answered the moment it is asked for, which is the fastest a
 * platform is allowed to be and the easiest to reason about. */
static std::vector<uint8_t> g_stage;

static void clk(void)
{
    if (dut->wiring_stage_pend)
    {
        uint32_t a = dut->wiring_stage_addr;
        dut->stage_rdata = a < g_stage.size() ? g_stage[a] : 0;
    }
    dut->stage_stall = 0;
    dut->eval();
    tb_clock(dut);
}

/* Where the host leaves a blob, in the store's own address space, with
 * the lowest address in the most significant byte. */
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

/* The same bytes, fetched a different way: straight out of the model's
 * arrays rather than through the engine and the machine's bus. */
/* A blob is not believed until it adds up, so the bench has to make one
 * that does: the same rolling sum the engine writes into the trailer,
 * over every word ahead of it. */
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

/* XRAM is four byte lanes indexed by the low two address bits. Its
 * blob region is the array's own word order -- byte 0 in the low
 * lane, as the render reads it -- the same both directions, which is
 * all a round trip asks. Only the byte-stream regions (SRAM, and the
 * staging store a blob arrives through) put the lowest address in the
 * most significant byte. */
static uint32_t xram_word(uint32_t w)
{
    auto *r = dut->rootp;
    return ((uint32_t)r->wiring__DOT__xram__DOT__mem3[w] << 24)
           | ((uint32_t)r->wiring__DOT__xram__DOT__mem2[w] << 16)
           | ((uint32_t)r->wiring__DOT__xram__DOT__mem1[w] << 8)
           | (uint32_t)r->wiring__DOT__xram__DOT__mem0[w];
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
    dut->stage_rdata = 0;
    dut->eval();
    for (uint32_t i = 0; i < COUNTER.size(); i++)
        tcm_put(i, COUNTER[i]);
    clk();
    clk();
    dut->rst_n = 1;
}

/* Ask for a savestate and wait for the machine to come to rest. The
 * wait is long because the engine adds the whole blob up before it says
 * a word of it can be read: the trailer has to be a fact about the blob
 * rather than about the order somebody read it in. */
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
    /* The index is data; the toggle beside it is what says it has
     * stopped moving, which is the only thing the engine acts on. */
    dut->sst_rd_idx = idx;
    dut->sst_rd_t = !dut->sst_rd_t;
    dut->eval();
    /* The answer to the last index is still standing while the engine
     * notices this one, so wait for it to go away first. */
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
    /* Both halves are stopped, or the blob would move underneath it. */
    ASSERT_TRUE((int)dut->wiring_sst_stop_req);
    ASSERT_TRUE((int)dut->wiring_sst_dbg_halted);

    /* And they stay stopped while the blob is being read. */
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

    /* The trailer must be the sum of the words as served, or a restore
     * refuses the machine's own blob. Walking all eighty-one thousand
     * words here costs seconds and catches a divergence between the
     * checksum pass and the serving path exactly where it happens. */
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

/* The scanline program is the one thing in the machine that nothing
 * could read back: four arrays with a writer and a reader each, and
 * both readers belong to the render. The engine borrows them, which it
 * is allowed to do because it has stopped the render to get there. */
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
        /* The sprite enable is twenty bits kept as a top bit and
         * nineteen below it, which is how the render reads it. */
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

    /* Composed from the model's own flops rather than read back
     * through the state port, which the engine is holding. */
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

    /* The VIA's timers, which nothing else can read at all. */
    ASSERT_TRUE(blob_word(B_STATE + ST_VIA + 2, &w));
    ASSERT_EQ((uint32_t)r->wiring__DOT__via__DOT__t1_latch
                  | ((uint32_t)r->wiring__DOT__via__DOT__t1_counter << 16),
              w);
    ASSERT_TRUE(blob_word(B_STATE + ST_VIA + 4, &w));
    ASSERT_EQ((uint32_t)r->wiring__DOT__via__DOT__t2_pip
                  | ((uint32_t)r->wiring__DOT__via__DOT__t1_pip << 16),
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
    ASSERT_TRUE(blob_word(B_STATE + ST_RV + 5, &x5));
    ASSERT_TRUE(blob_word(B_STATE + ST_RV + 6, &x6));
    ASSERT_TRUE(blob_word(B_STATE + ST_RV + 0, &dpc));
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
    /* How far the program had got. The strict-advance check below is
     * the whole test: every assertion the old version made was
     * satisfiable by a soft CPU that stayed halted forever, which is
     * exactly the bug the engine had -- dropping the halt request is
     * not a resume. */
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
    /* It carried on from where it was rather than starting over. */
    ASSERT_EQ(held, tcm_word(256));
    uint32_t count = 0;
    for (uint32_t i = 0; i < W_TCM && tcm_word(256 + i); i++)
        count++;
    ASSERT_GT(count, count_before);
    for (uint32_t i = 0; i < count; i++)
        ASSERT_EQ(i + 1, tcm_word(256 + i));
}

/* The other direction: the blob is already in the store, so the engine
 * reads it back through the machine's own window and writes it where
 * each index says. Nothing about it involves the host. */
UTEST(sst, a_load_puts_the_blob_back)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();

    /* Every window the blob writes through, each recognisably its own:
     * the two byte windows, the word window the cells answer on, the
     * soft CPU's memory, and the flops that are on no window at all. */
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
    /* The 6502 out of reset and running at a rate nothing else would
     * have chosen, then what it was holding. Its program counter is
     * aimed at a run of WAI opcodes staged into SRAM, because after
     * the release the machine is genuinely running -- the only way to
     * assert its registers is to have it resume into an instruction
     * that holds them still. */
    stage_word(B_STATE + ST_MACH + 0, 1);
    stage_word(B_STATE + ST_MACH + 1, 3000);
    stage_word(B_STATE + ST_W65C02 + 0, 0x11223344u);
    stage_word(B_STATE + ST_W65C02 + 1, 0xA5B50010u);
    stage_word(B_STATE + ST_W65C02 + 4, 0x00100001u);
    stage_word(B_STATE + ST_VIA + 2, 0xBEEF1234u);
    for (uint32_t i = 4; i < 8; i++)
        stage_word(B_SRAM + i, 0xCBCBCBCBu);
    stage_seal();

    /* Scribble over the destinations first, so a load that did nothing
     * would be caught. */
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
        /* The byte window takes the lowest address from the top of the
         * word, which is the order the store hands it back in. */
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
        /* The cells answer as a word, so the lanes go back as they came. */
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

    /* Before the release, the flops still hold the old world: the jam
     * is the release's consequence, because the 6502's async reset
     * dominates any jam attempted while it is still held. */
    ASSERT_NE(0x44u, (uint32_t)r->wiring__DOT__cpu__DOT__a);

    dut->sst_load = 0;
    dut->eval();
    for (int i = 0; i < 200; i++)
        clk();
    ASSERT_FALSE((int)dut->wiring_sst_stop_req);

    /* The flops, jammed on the release with the resets let go first. */
    ASSERT_EQ(0x44u, (uint32_t)r->wiring__DOT__cpu__DOT__a);
    ASSERT_EQ(0x33u, (uint32_t)r->wiring__DOT__cpu__DOT__x);
    ASSERT_EQ(0x22u, (uint32_t)r->wiring__DOT__cpu__DOT__y);
    ASSERT_EQ(0x11u, (uint32_t)r->wiring__DOT__cpu__DOT__s);
    ASSERT_EQ(0xB5u, (uint32_t)r->wiring__DOT__cpu__DOT__p);
    ASSERT_EQ(0x1234u, (uint32_t)r->wiring__DOT__via__DOT__t1_latch);

    /* The machine's own flops have no other way out, so the proof they
     * landed is the next blob carrying them. */
    ASSERT_TRUE(begin_save());
    uint32_t w = 0;
    ASSERT_TRUE(blob_word(B_STATE + ST_MACH + 0, &w));
    ASSERT_EQ(1u, w);
    ASSERT_TRUE(blob_word(B_STATE + ST_MACH + 1, &w));
    ASSERT_EQ(3000u, w);
}

/* The registers are the half of a restore that no memory carries. They
 * go back in through the same debug port they came out of, and the only
 * proof they landed is the soft CPU doing something with them: it is
 * put down in front of a store instruction with a pointer and a value
 * it never computed, and what it writes says whether it believed them.
 *
 * The blob's copy of the program's records is blank and the pointer is
 * put where the program could never have reached, so a core that
 * started over writes in one place and a core that resumed writes in
 * the other, and neither can be mistaken for the other. */
UTEST(sst, the_soft_cpus_registers_go_back_in)
{
    power_on();
    for (int i = 0; i < 3000; i++)
        clk();

    /* The blob keeps the program, because a load overwrites the soft
     * CPU's memory along with everything else and a core resumed into
     * an erased one has nowhere to go. */
    g_stage.assign(STAGE_BLOB + (W_TOTAL + 4) * 4, 0);
    for (uint32_t i = 0; i < W_TOTAL; i++)
        stage_word(i, 0);
    for (uint32_t i = 0; i < COUNTER.size(); i++)
        stage_word(B_TCM + i, COUNTER[i]);

    /* Stopped in front of `sw x5, 0(x6)` -- the third word of the loop
     * -- holding a pointer and a value of our choosing. */
    const uint32_t MARK = 0x11110000u;
    const uint32_t PTR = 0x4000u;
    stage_word(B_STATE + ST_RV + 0, 0x14u);
    stage_word(B_STATE + ST_RV + 5, MARK);
    stage_word(B_STATE + ST_RV + 6, PTR);
    /* The soft CPU is the thing being resumed, so the machine it runs
     * has to be out of reset for the record to mean anything. */
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

    /* It took the pointer and the value, and then went on around the
     * loop from there. */
    ASSERT_EQ(MARK, tcm_word(PTR / 4));
    ASSERT_EQ(MARK + 1, tcm_word(PTR / 4 + 1));
    /* And it did not start over: the two words the program writes on
     * its way into the loop are still as the blob left them. */
    ASSERT_EQ(0u, tcm_word(255));
    ASSERT_EQ(0u, tcm_word(256));
}

/* A restore is the one operation with nothing to fall back on: it
 * overwrites every memory the machine has, so a blob that turns out to
 * be wrong halfway through leaves nothing to be wrong about. The whole
 * of it is therefore read and added up before a byte of it is written,
 * and what does not add up is refused with the machine still standing.
 *
 * The two ways a blob goes wrong are not the same. A header that is not
 * a header is caught on the first word. A body that has been changed is
 * caught only by the sum, and that is the case the sum is for -- the
 * host reads the blob out of the machine in whatever order it likes,
 * and a blob served out of order is a blob whose words are all present
 * and in the wrong places. */
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
        /* After power_on, which builds a new model: the old root is
         * freed and writing through it is writing nowhere. */
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

        /* Nothing written. */
        for (uint32_t i = 0; i < 8; i++)
            ASSERT_EQ(0xEEEEEEEEu, xram_word(i));

        /* And the soft CPU was only ever halted, so letting it go puts
         * it back where the halt found it. */
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

/* The console queue, which a sleep used to drop.
 *
 * Sixteen bytes the 6502 has written and the soft CPU has not taken
 * yet. They live in the regs window at word 16, and reading that word
 * is what takes a byte off the queue -- so the savestate cannot read
 * it, and for two versions of the blob it did not read anything else
 * either and the queue went out with the session. It has its own words
 * now, which disturb nothing: 20 for the pointers and 21 to 24 for the
 * bytes. Staged in, released, and asked for again. */
UTEST(sst, the_console_queue_survives)
{
    power_on();
    for (int i = 0; i < 2000; i++)
        clk();

    g_stage.assign(STAGE_BLOB + (W_TOTAL + 4) * 4, 0);
    for (uint32_t i = 0; i < W_TOTAL; i++)
        stage_word(i, 0);
    /* Full, and every byte distinct, so a queue put back rotated or
     * half-written is a different answer and not a lucky one. Read at
     * 5, write wrapped round to it, which is what a full queue looks
     * like and the one arrangement an off-by-one gets wrong. */
    stage_word(B_REGS + 20, (16u << 8) | (5u << 4) | 5u);
    stage_word(B_REGS + 21, 0xD3C2B1A0u);
    stage_word(B_REGS + 22, 0xD7C6B5A4u);
    stage_word(B_REGS + 23, 0xDBCAB9A8u);
    stage_word(B_REGS + 24, 0xDFCEBDACu);
    /* The 6502 stays in reset: a queue is only still the queue if
     * nothing is pushing to it while this looks. */
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
