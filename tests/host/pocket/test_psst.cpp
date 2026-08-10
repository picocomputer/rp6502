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
#define REG_RESULT 0x04u

#define CTL_START_REQ (1u << 0)
#define CTL_LOAD_REQ (1u << 1)
#define CTL_BLOB_SEEN (1u << 2)

#define RES_START_OK 1u
#define RES_START_ERR 2u
#define RES_LOAD_OK 3u
#define RES_LOAD_ERR 4u

static Vpocket_sst *dut;
static long t;

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

UTEST(psst, the_request_reaches_the_machine_and_clears)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(64);
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_START_REQ) != 0);
    mmio_write(REG_CTL, CTL_START_REQ);
    run(16);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_START_REQ) != 0);
}

UTEST(psst, the_two_requests_do_not_collide)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    run(64);
    uint32_t ctl = mmio_read(REG_CTL);
    ASSERT_TRUE((ctl & CTL_LOAD_REQ) != 0);
    ASSERT_FALSE((ctl & CTL_START_REQ) != 0);
}

UTEST(psst, a_result_ends_busy_and_stands)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(64);
    mmio_write(REG_RESULT, RES_START_OK);
    run(64);
    ASSERT_FALSE((int)dut->pocket_sst_start_busy);
    ASSERT_TRUE((int)dut->pocket_sst_start_ok);
    ASSERT_FALSE((int)dut->pocket_sst_start_err);
    /* Held, because the host polls for it in its own time. */
    run(400);
    ASSERT_TRUE((int)dut->pocket_sst_start_ok);
}

UTEST(psst, an_error_is_reported_as_an_error)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(64);
    mmio_write(REG_RESULT, RES_START_ERR);
    run(64);
    ASSERT_TRUE((int)dut->pocket_sst_start_err);
    ASSERT_FALSE((int)dut->pocket_sst_start_ok);
    ASSERT_FALSE((int)dut->pocket_sst_start_busy);
}

UTEST(psst, a_new_create_clears_the_last_result)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(64);
    mmio_write(REG_RESULT, RES_START_OK);
    run(64);
    ASSERT_TRUE((int)dut->pocket_sst_start_ok);
    ASSERT_TRUE(hold_until_ack(0));
    ASSERT_FALSE((int)dut->pocket_sst_start_ok);
    ASSERT_TRUE((int)dut->pocket_sst_start_busy);
}

UTEST(psst, a_load_result_does_not_answer_a_create)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    run(64);
    mmio_write(REG_RESULT, RES_LOAD_OK);
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
