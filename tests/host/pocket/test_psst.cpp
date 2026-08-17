/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The savestate bridge, with the bench playing the host on one side and
 * the firmware on the other.
 *
 * The handshake is the part that cannot be got wrong: the bridge's
 * command engine parks in the savestate state until the ack, so a core
 * that claims support and does not answer takes every host command down
 * with it. The other half is the sticky bit that says a blob is
 * arriving, which is how a wake announces itself before any command
 * does.
 */

#include "Vpocket_sst.h"

#include "utest.h"

#include <vector>

/* clk_74a every second unit, clk_sys every third: 1.5, against the
 * 74.25 and 50.4 MHz the board runs them at. */
#define BRG_PERIOD 2
#define SYS_PERIOD 3

#define BLOB_BASE 0x03F00000u
#define BLOB_WINDOW 0x000A0000u

#define REG_CTL 0x00u

#define CTL_RESTORED (1u << 0)
#define CTL_BLOB_SEEN (1u << 1)
#define CTL_UNDERRUN (1u << 2)
#define CTL_RESTORE_ERR (1u << 3)

/* pocket_sst's BLOB_WORDS. Reading the last of them is what ends a
 * create, because there is no host command that says so. */
#define BLOB_WORDS 81236u

static Vpocket_sst *dut;
static long t;

/* The engine, played by the bench: it holds a word for whatever index
 * is asked of it and answers after a few clocks, which is what the
 * prefetch is written against. */
static uint32_t eng_word(uint32_t idx) { return 0xA5000000u | idx; }

static int eng_delay;
static int eng_seen_t;
static bool eng_stopped;
static int eng_freeze;

/* The real engine says nothing until it has stopped the machine, so
 * neither does this: ready and the answer both stay low until the
 * freeze it was asked for has happened.
 *
 * It re-arms on the toggle beside the index and not on the index
 * changing, because that is what the engine does -- the same address
 * asked for twice is two requests, and a model that noticed only the
 * address would leave its answer standing and make the second one look
 * like it had been served before it was asked. */
static void eng_step(void)
{
    if (eng_stopped || !dut->pocket_sst_save)
    {
        eng_freeze = 40;
        dut->sst_rvalid = 0;
        dut->sst_ready = 0;
        eng_seen_t = -1;
        return;
    }
    if (eng_freeze > 0)
    {
        eng_freeze--;
        dut->sst_rvalid = 0;
        dut->sst_ready = 0;
        return;
    }
    dut->sst_ready = 1;
    if ((int)dut->pocket_sst_rd_t != eng_seen_t)
    {
        eng_seen_t = (int)dut->pocket_sst_rd_t;
        eng_delay = 6;
        dut->sst_rvalid = 0;
    }
    else if (eng_delay > 0 && --eng_delay == 0)
    {
        dut->sst_rdata = eng_word(dut->pocket_sst_rd_idx);
        dut->sst_rvalid = 1;
    }
}

static void tick(void)
{
    t++;
    if (t % SYS_PERIOD == 0)
    {
        dut->eval();
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
    }
    if (t % BRG_PERIOD == 0)
    {
        eng_step();
        dut->eval();
        dut->clk_74a = 1;
        dut->eval();
        dut->clk_74a = 0;
        dut->eval();
    }
}

static void run(long units)
{
    for (long i = 0; i < units; i++)
        tick();
}

static void reset(void)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vpocket_sst;
    t = 0;
    dut->clk_sys = 0;
    dut->clk_74a = 0;
    dut->arst_n = 0;
    dut->stb = 0;
    dut->we = 0;
    dut->addr = 0;
    dut->wdata = 0;
    dut->bridge_wr = 0;
    dut->bridge_addr = 0;
    dut->savestate_start = 0;
    dut->savestate_load = 0;
    dut->bridge_rd = 0;
    dut->sst_ready = 0;
    dut->sst_rvalid = 0;
    dut->sst_rdata = 0;
    dut->sst_load_done = 0;
    dut->sst_load_err = 0;
    eng_delay = 0;
    eng_seen_t = -1;
    eng_stopped = false;
    eng_freeze = 40;
    dut->eval();
    run(16);
    dut->arst_n = 1;
    run(16);
}

/* The firmware's side: one bus access, landing on a clk_sys edge. */
static void mmio_write(uint32_t off, uint32_t val)
{
    dut->addr = off;
    dut->wdata = val;
    dut->we = 1;
    dut->stb = 1;
    do
        tick();
    while (t % SYS_PERIOD != 0);
    dut->stb = 0;
    dut->we = 0;
    tick();
}

static uint32_t mmio_read(uint32_t off)
{
    dut->addr = off;
    dut->we = 0;
    dut->stb = 1;
    do
        tick();
    while (t % SYS_PERIOD != 0);
    dut->stb = 0;
    tick();
    return dut->pocket_sst_rdata;
}

/* Only that a write landed, and where. What the host put there goes to
 * the staging store through pocket_bridge and is no business of this
 * module's. */
static void bridge_write(uint32_t byte_addr)
{
    dut->bridge_addr = byte_addr;
    dut->bridge_wr = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_wr = 0;
    tick();
}

/* Raise the command level and hold it until the core acks, which is
 * what the bridge's command engine does. Fails rather than hangs. */
static int hold_until_ack(int is_load)
{
    if (is_load)
        dut->savestate_load = 1;
    else
        dut->savestate_start = 1;
    for (int i = 0; i < 64; i++)
    {
        tick();
        int ack = is_load ? dut->pocket_sst_load_ack : dut->pocket_sst_start_ack;
        if (ack)
        {
            run(4);
            if (is_load)
                dut->savestate_load = 0;
            else
                dut->savestate_start = 0;
            run(8);
            return 1;
        }
    }
    if (is_load)
        dut->savestate_load = 0;
    else
        dut->savestate_start = 0;
    return 0;
}

/* One bridge read at a blob address, sampled the way the host does:
 * present the address, then look a few clocks later. */
/* The read contract, quoted from the bus documentation: "Upon receiving
 * a read the core may not immediately provide the read data and has up
 * until the next read strobe to drive bridge_rd_data." So the strobe
 * asks, and the word is taken later -- before the next strobe goes out.
 *
 * How much later is not published as a number. The same page gives the
 * bus as "a few megabytes per second", which is about a microsecond for
 * a thirty-two bit word, and that is where this gap comes from: a model
 * of the host, not a rule it stated. */
#define HOST_GAP 74

static uint32_t blob_read(uint32_t idx)
{
    dut->bridge_addr = BLOB_BASE + idx * 4;
    dut->bridge_rd = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_rd = 0;
    run(HOST_GAP * BRG_PERIOD);
    return dut->pocket_sst_rd_data;
}

UTEST(psst, the_blob_comes_out_in_order)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    for (uint32_t i = 0; i < 16; i++)
        ASSERT_EQ(eng_word(i), blob_read(i));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_UNDERRUN) != 0);
}

UTEST(psst, a_repeated_address_is_served_the_same_word)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    ASSERT_EQ(eng_word(0), blob_read(0));
    /* The host re-reads; it must not be given the next word. */
    ASSERT_EQ(eng_word(0), blob_read(0));
    ASSERT_EQ(eng_word(0), blob_read(0));
    ASSERT_EQ(eng_word(1), blob_read(1));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_UNDERRUN) != 0);
}

/* Every word answered inside the gap the host leaves, which is all
 * "up until the next read strobe" asks of the core. */
UTEST(psst, it_answers_inside_the_gap)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    for (uint32_t i = 0; i < 40; i++)
        ASSERT_EQ(eng_word(i), blob_read(i));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_UNDERRUN) != 0);
}

/* Any address, in any order, is a legal thing for the host to ask for:
 * nothing published says it reads the blob start to end, and a core
 * that only worked if it did would be trusting a guess. What is not
 * legal is strobing again before the core has driven the last word, and
 * that the core does report rather than let a stale word pass. */
UTEST(psst, any_address_is_answered_and_a_rushed_strobe_is_not)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    ASSERT_EQ(eng_word(900), blob_read(900));
    ASSERT_EQ(eng_word(3), blob_read(3));
    ASSERT_EQ(eng_word(1000), blob_read(1000));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_UNDERRUN) != 0);

    dut->bridge_addr = BLOB_BASE + 4 * 4;
    dut->bridge_rd = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_rd = 0;
    run(2 * BRG_PERIOD);
    (void)blob_read(5);
    run(32);
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_UNDERRUN) != 0);
}

UTEST(psst, a_create_is_acked_without_the_firmware)
{
    reset();
    /* Nothing is polling the machine side; the ack still has to come or
     * the bridge parks and every host command after it is lost. */
    ASSERT_TRUE(hold_until_ack(0));
    ASSERT_TRUE((int)dut->pocket_sst_start_busy);
    ASSERT_FALSE((int)dut->pocket_sst_start_ok);
    ASSERT_FALSE((int)dut->pocket_sst_start_err);
}

UTEST(psst, a_load_is_acked_without_the_firmware)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    ASSERT_TRUE((int)dut->pocket_sst_load_busy);
}

/* The soft CPU is stopped along with the rest of the machine while a
 * blob is made, so there is no firmware to answer a create and the
 * fabric answers it: busy until the engine has the machine stopped and
 * can be read, done from then on. */
UTEST(psst, a_create_is_answered_without_the_firmware)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    ASSERT_TRUE((int)dut->pocket_sst_start_busy);
    ASSERT_FALSE((int)dut->pocket_sst_start_ok);
    run(200);
    ASSERT_FALSE((int)dut->pocket_sst_start_busy);
    ASSERT_TRUE((int)dut->pocket_sst_start_ok);
    ASSERT_FALSE((int)dut->pocket_sst_start_err);
    /* Held, because the host polls for it in its own time. */
    run(400);
    ASSERT_TRUE((int)dut->pocket_sst_start_ok);
}

/* And it ends when the host has taken the last word, which is the only
 * signal there is that the machine is no longer being read. */
UTEST(psst, the_last_word_gives_the_machine_back)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    ASSERT_TRUE((int)dut->pocket_sst_save);
    (void)blob_read(BLOB_WORDS - 2);
    run(16);
    ASSERT_TRUE((int)dut->pocket_sst_save);
    (void)blob_read(BLOB_WORDS - 1);
    run(32);
    ASSERT_FALSE((int)dut->pocket_sst_save);
}

UTEST(psst, the_two_requests_do_not_collide)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    run(64);
    ASSERT_TRUE((int)dut->pocket_sst_load_busy);
    ASSERT_FALSE((int)dut->pocket_sst_start_busy);
    ASSERT_FALSE((int)dut->pocket_sst_save);
}

/* A load is not a request the firmware services; it is one the engine
 * services and the firmware is told about afterwards, because the
 * firmware that is told is the one the blob brought. So the host's
 * command goes to the engine, and the bit the firmware reads is the
 * engine's answer -- which is also why clearing that bit is what lets
 * the machine run again. */
UTEST(psst, a_load_goes_to_the_engine_and_the_answer_comes_back)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    run(64);
    ASSERT_TRUE((int)dut->pocket_sst_load);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_RESTORED) != 0);

    ASSERT_TRUE((int)dut->pocket_sst_load_busy);

    dut->sst_load_done = 1;
    run(16);
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_RESTORED) != 0);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_RESTORE_ERR) != 0);
    ASSERT_TRUE((int)dut->pocket_sst_load_ok);
    ASSERT_FALSE((int)dut->pocket_sst_load_busy);

    mmio_write(REG_CTL, CTL_RESTORED);
    run(16);
    ASSERT_FALSE((int)dut->pocket_sst_load);
    /* The host's answer stands even after the firmware has let go. */
    ASSERT_TRUE((int)dut->pocket_sst_load_ok);
}

UTEST(psst, a_refused_load_says_so)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    run(64);
    dut->sst_load_done = 1;
    dut->sst_load_err = 1;
    run(16);
    uint32_t ctl = mmio_read(REG_CTL);
    ASSERT_TRUE((ctl & CTL_RESTORED) != 0);
    ASSERT_TRUE((ctl & CTL_RESTORE_ERR) != 0);
    ASSERT_TRUE((int)dut->pocket_sst_load_err);
    ASSERT_FALSE((int)dut->pocket_sst_load_ok);
}

/* The one way a create can go wrong: a word the engine had not got to.
 * The blob the host took is short, so it must not be told the state is
 * good. */
UTEST(psst, a_late_answer_is_reported_as_an_error)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    ASSERT_TRUE((int)dut->pocket_sst_start_ok);
    dut->bridge_addr = BLOB_BASE;
    dut->bridge_rd = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_rd = 0;
    run(2 * BRG_PERIOD);
    (void)blob_read(1);
    run(32);
    ASSERT_TRUE((int)dut->pocket_sst_start_err);
    ASSERT_FALSE((int)dut->pocket_sst_start_ok);
}

/* A second create after the machine has been given back starts over:
 * busy while the engine stops it again, and none of the last one's
 * answer left standing. An underrun in particular must not carry, or
 * every state after a bad one reads as bad. */
UTEST(psst, a_new_create_clears_the_last_result)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    dut->bridge_addr = BLOB_BASE;
    dut->bridge_rd = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_rd = 0;
    run(2 * BRG_PERIOD);
    (void)blob_read(1);
    run(32);
    ASSERT_TRUE((int)dut->pocket_sst_start_err);
    (void)blob_read(BLOB_WORDS - 1);
    run(32);
    ASSERT_FALSE((int)dut->pocket_sst_save);

    ASSERT_TRUE(hold_until_ack(0));
    ASSERT_FALSE((int)dut->pocket_sst_start_ok);
    ASSERT_FALSE((int)dut->pocket_sst_start_err);
    ASSERT_TRUE((int)dut->pocket_sst_start_busy);
    run(200);
    ASSERT_TRUE((int)dut->pocket_sst_start_ok);
    ASSERT_FALSE((int)dut->pocket_sst_start_err);
}

UTEST(psst, a_restore_does_not_answer_a_create)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    dut->sst_load_done = 1;
    run(64);
    ASSERT_TRUE((int)dut->pocket_sst_load_ok);
    ASSERT_FALSE((int)dut->pocket_sst_start_ok);
    ASSERT_FALSE((int)dut->pocket_sst_load_busy);
}

UTEST(psst, a_write_into_the_window_is_a_blob_arriving)
{
    reset();
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_BLOB_SEEN) != 0);
    bridge_write(BLOB_BASE + 0x40);
    run(64);
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_BLOB_SEEN) != 0);
}

UTEST(psst, a_write_outside_the_window_is_not)
{
    reset();
    /* The ROM slot below it, and the first file window above it. */
    bridge_write(BLOB_BASE - 4);
    bridge_write(BLOB_BASE + BLOB_WINDOW);
    run(64);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_BLOB_SEEN) != 0);
}

UTEST_MAIN()
