/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vopl.h"

#include "utest.h"

#include <cstdint>
#include <cstdlib>

extern "C"
{
#include "core/aud/opl.h"
}

static Vopl *dut;

static void tick()
{
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

/* fresh() builds a new model rather than resetting the old one, because
 * the reset of the OPL2 core inside opl.sv clears the core's key-on memory
 * and none of its other register memories. A note that was sounding goes
 * into release at the release rate still in its register, so a slow
 * release is still sounding when the next test starts. */
static void fresh()
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vopl;
    dut->clk = 0;
    dut->xaddr_we = 0;
    dut->q_we = 0;
    dut->eval();
    /* chip_rst in opl.sv starts at 255, so opl.sv holds the core's reset
     * input, ic_n, low for the first 255 clocks. */
    for (int i = 0; i < 302; i++)
        tick();
}

static void set_page(uint16_t word)
{
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    tick();
    dut->xaddr_we = 0;
    /* A pointer write reloads chip_rst to 255, and opl.sv drops every
     * register write until chip_rst has counted down to zero. */
    for (int i = 0; i < 300; i++)
        tick();
}

/* opl.sv takes four clocks after a write to pass it to the core as a
 * register select and then a value, and it drops a write that arrives
 * before it is idle again. */
static void poke(uint16_t page, uint8_t reg, uint8_t val)
{
    dut->q_we = 1;
    dut->q_addr = (uint16_t)((page << 8) | reg);
    dut->q_val = val;
    tick();
    dut->q_we = 0;
    for (int i = 0; i < 8; i++)
        tick();
}

static uint64_t last_peak;

static uint64_t energy(int samples)
{
    uint64_t sum = 0;
    int seen = 0;
    last_peak = 0;
    for (long i = 0; seen < samples && i < 40000000L; i++)
    {
        tick();
        if (dut->opl_valid)
        {
            uint64_t d = (uint64_t)std::abs((int)(int16_t)dut->opl_out);
            sum += d;
            if (d > last_peak)
                last_peak = d;
            seen++;
        }
    }
    return sum;
}

/* Channel 0's modulator is operator slot 0 and its carrier is slot 3. */
static const uint8_t note[11][2] = {
    {0x20, 0x01}, /* modulator: mult 1              */
    {0x23, 0x01}, /* carrier:   mult 1              */
    {0x40, 0x10}, /* modulator: total level 16      */
    {0x43, 0x00}, /* carrier:   full volume         */
    {0x60, 0xF0}, /* fast attack, no decay          */
    {0x63, 0xF0},
    {0x80, 0x77}, /* sustain level 7, release 7     */
    {0x83, 0x77},
    {0xC0, 0x0E}, /* feedback 7, FM connection      */
    {0xA0, 0x98}, /* f-number low                   */
    {0xB0, 0x31}, /* key on, block 4, f-number high */
};

static void note_on(uint16_t page)
{
    for (const auto &w : note)
        poke(page, w[0], w[1]);
}

/* opl.c's peak over the samples a_note_makes_sound measures, after the same
 * note. */
static uint64_t c_peak()
{
    opl_reset();
    for (const auto &w : note)
        opl_push(w[0], w[1]);
    for (int i = 0; i < 64; i++)
        opl_sample();
    uint64_t peak = 0;
    for (int i = 0; i < 512; i++)
    {
        uint64_t d = (uint64_t)std::abs((int)opl_sample());
        if (d > peak)
            peak = d;
    }
    return peak;
}

UTEST(opl, silent_until_the_pointer_is_programmed)
{
    fresh();
    note_on(0x12);
    ASSERT_FALSE(dut->opl_enabled);
    ASSERT_EQ(energy(64), (uint64_t)0);
}

UTEST(opl, a_note_makes_sound)
{
    fresh();
    set_page(0x1200);
    ASSERT_TRUE(dut->opl_enabled);
    note_on(0x12);
    energy(64);
    ASSERT_GT(energy(512), (uint64_t)512);
    /* This engine's peak is held to opl.c's, which is a real YM3812's. */
    const uint64_t c = c_peak();
    fprintf(stderr, "  opl peak %llu, opl.c %llu\n",
            (unsigned long long)last_peak, (unsigned long long)c);
    ASSERT_GT(last_peak, c - c / 16);
    ASSERT_LT(last_peak, c + c / 16);
}

UTEST(opl, the_pointer_gates_the_page_it_names)
{
    fresh();
    set_page(0x1200);
    note_on(0x34);
    energy(64);
    ASSERT_EQ(energy(256), (uint64_t)0);
}

UTEST(opl, ffff_puts_it_away)
{
    fresh();
    set_page(0x1200);
    note_on(0x12);
    energy(64);
    ASSERT_GT(energy(256), (uint64_t)256);

    set_page(0xFFFF);
    ASSERT_FALSE(dut->opl_enabled);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
