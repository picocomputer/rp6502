/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The OPL2 device, checked the way audio is worth checking here: the
 * vendored core has its own suite and its own reference captures, and
 * there is no emu8950 agreement to hold it to, so nothing below asserts
 * a waveform. What it asserts is the wiring — that the page pointer
 * gates the engine, that a register written into the XRAM page reaches
 * the chip as a register write, and that a note asked for is a note
 * heard.
 *
 * Silence is the interesting failure. A snoop decoded a byte wrong, a
 * host strobe never seen as an edge, a pointer compared against the
 * wrong half of the address all land the same way: samples that never
 * leave the centre. So energy away from 512 is the measurement.
 */

#include "Vaud_opl.h"

#include "utest.h"

#include <cstdint>
#include <cstdlib>

static Vaud_opl *dut;

static void tick()
{
    dut->clk = 1;
    dut->eval();
    dut->clk = 0;
    dut->eval();
}

/* A new device rather than a reset one. The core's /IC clears what is
 * sounding but not every byte of its register file -- only one of its
 * banks is the reset-walking kind -- so a note left ringing with a slow
 * release carries its tail into whatever runs next. Real programs write
 * every register they use and never see this; tests that share one
 * instance do. */
static void fresh()
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vaud_opl;
    dut->clk = 0;
    dut->xaddr_we = 0;
    dut->q_we = 0;
    dut->eval();
    /* chip_rst powers up full, so the core's IC runs itself out. */
    for (int i = 0; i < 302; i++)
        tick();
}

static void set_page(uint16_t word)
{
    dut->xaddr_we = 1;
    dut->xaddr_wdata = word;
    tick();
    dut->xaddr_we = 0;
    /* The pointer write resets the chip, and that reset walks the
     * register file rather than clearing it, so nothing may be poked
     * until it lands. */
    for (int i = 0; i < 300; i++)
        tick();
}

/* A write into the device's XRAM page, exactly as the RW engine would
 * present it. The sequencer needs four clocks to turn one of these into
 * the chip's two-step address-then-data write; the 6502 could not
 * deliver them faster than that anyway. */
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

/* Sum of |sample| over a span, which is silence at zero and
 * anything at all otherwise, and the loudest excursion in that span.
 *
 * The sum alone is a bad test and was one: it passed a voice sitting 18
 * dB below where it belonged, because a few hundred samples deviating
 * by a single count still sum to more than a few hundred. Level is the
 * thing that separates working audio from audio you cannot hear, so
 * peak is what the loud case asserts. */
static uint64_t last_peak;

static uint64_t energy(int samples)
{
    uint64_t sum = 0;
    int seen = 0;
    last_peak = 0;
    /* A sample every CLK_DIV_COUNT clocks, with room for the envelope
     * to climb out of its attack. */
    for (long i = 0; seen < samples && i < 40000000L; i++)
    {
        tick();
        if (dut->aud_opl_valid)
        {
            uint64_t d = (uint64_t)std::abs((int)(int16_t)dut->aud_opl_out);
            sum += d;
            if (d > last_peak)
                last_peak = d;
            seen++;
        }
    }
    return sum;
}

/* Channel 0's modulator is slot 0 and its carrier slot 3. */
static void note_on(uint16_t page)
{
    poke(page, 0x20, 0x01); /* modulator: mult 1              */
    poke(page, 0x23, 0x01); /* carrier:   mult 1              */
    poke(page, 0x40, 0x10); /* modulator: a little attenuated */
    poke(page, 0x43, 0x00); /* carrier:   full volume         */
    poke(page, 0x60, 0xF0); /* fast attack, no decay          */
    poke(page, 0x63, 0xF0);
    poke(page, 0x80, 0x77); /* sustain high, slow release     */
    poke(page, 0x83, 0x77);
    poke(page, 0xC0, 0x0E); /* feedback, both operators out   */
    poke(page, 0xA0, 0x98); /* f-number low                   */
    poke(page, 0xB0, 0x31); /* key on, block 4, f-number high */
}

UTEST(opl, silent_until_the_pointer_is_programmed)
{
    fresh();
    /* Writes land in XRAM whether or not the device is pointed at that
     * page; only the pointer decides whether the engine hears them. */
    note_on(0x12);
    ASSERT_FALSE(dut->aud_opl_enabled);
    ASSERT_EQ(energy(64), (uint64_t)0);
}

UTEST(opl, a_note_makes_sound)
{
    fresh();
    set_page(0x1200);
    ASSERT_TRUE(dut->aud_opl_enabled);
    note_on(0x12);
    /* Past the attack, then measure. The level is the point: this engine
     * and core/aud/opl.c are the same chip twice and have to be the same
     * loudness, and nothing else compares them. This one sets the level —
     * an RTL YM3812 is the closer thing to the chip — and opl.c's *4 is
     * emu8950 coming up to meet it, from a peak of 2043 to 8172.
     *
     * The bound used to be 128 out of "the 511 available", written when
     * the path was ten bits. It survived the widening to sixteen and was
     * then loose by a factor of five hundred, which is how this engine
     * came to run 6 dB hot with every test green. */
    energy(64);
    ASSERT_GT(energy(512), (uint64_t)512);
    fprintf(stderr, "  opl peak %llu (emu8950 with these registers: 8172)\n",
            (unsigned long long)last_peak);
    ASSERT_GT(last_peak, (uint64_t)6000);
    ASSERT_LT(last_peak, (uint64_t)11000);
}

UTEST(opl, the_pointer_gates_the_page_it_names)
{
    fresh();
    set_page(0x1200);
    /* Same registers, a page the device is not pointed at. */
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
    ASSERT_FALSE(dut->aud_opl_enabled);
    /* The engine keeps running; what stops is the machine listening,
     * which rp6502.sv decides from this bit. */
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
