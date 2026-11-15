/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vtb_psst.h"

#include "utest.h"

#include <vector>

/* clk_74a pulses every second unit and clk_sys every third, a ratio of
 * 1.5, where the board runs them at 74.25 MHz and 50.4 MHz. */
#define BRG_PERIOD 2
#define SYS_PERIOD 3

#define BLOB_BASE 0x03E80000u
#define BLOB_WINDOW 0x00080000u

#define REG_CTL 0x00u

#define CTL_RESTORED (1u << 0)
#define CTL_BLOB_SEEN (1u << 1)
#define CTL_UNDERRUN (1u << 2)
#define CTL_RESTORE_ERR (1u << 3)
#define CTL_LOAD_EARLY (1u << 4)
#define CTL_BLOB_UNASKED (1u << 5)
#define CTL_BLOB_WRITING (1u << 6)
#define CTL_WHY(ctl) (((ctl) >> 8) & 7u)

/* core_bridge_cmd's host command register and its first parameter, the
 * commands, and the result codes of the two savestate commands. */
#define HOST_CMD 0xF8000000u
#define HOST_PARAM 0xF8000020u
#define CMD_RESET_ENTER 0x0010u
#define CMD_RESET_EXIT 0x0011u
#define CMD_SAVE 0x00A0u
#define CMD_LOAD 0x00A4u
#define RES_IDLE 0u
#define RES_BUSY 1u
#define RES_OK 2u
#define RES_ERR 3u

#define BLOB_WORDS 75092u

static Vtb_psst *dut;
static long t;

static uint32_t eng_word(uint32_t idx) { return 0xA5000000u | idx; }

static int eng_delay;
static int eng_seen_t;
static bool eng_stopped;
static int eng_freeze;

static void eng_step(void)
{
    if (eng_stopped || !dut->tb_psst_save)
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
    if ((int)dut->tb_psst_rd_t != eng_seen_t)
    {
        eng_seen_t = (int)dut->tb_psst_rd_t;
        eng_delay = 6;
        dut->sst_rvalid = 0;
    }
    else if (eng_delay > 0 && --eng_delay == 0)
    {
        dut->sst_rdata = eng_word(dut->tb_psst_rd_idx);
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
    dut = new Vtb_psst;
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
    dut->bridge_wr_data = 0;
    dut->bridge_rd = 0;
    dut->sst_load_why = 0;
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
    return dut->tb_psst_rdata;
}

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

static void cmd_write(uint32_t addr, uint32_t w)
{
    dut->bridge_addr = addr;
    dut->bridge_wr_data = w;
    dut->bridge_wr = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_wr = 0;
    run(4 * BRG_PERIOD);
}

static uint32_t cmd_read(uint32_t addr)
{
    dut->bridge_addr = addr;
    dut->bridge_rd = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_rd = 0;
    run(2 * BRG_PERIOD);
    return dut->tb_psst_cmd_rd_data;
}

/* Sends a host command as the Pocket does and returns its result code,
 * or -1 when core_bridge_cmd never finishes it. */
static int host_cmd(uint32_t cmd, uint32_t param)
{
    cmd_write(HOST_PARAM, param);
    cmd_write(HOST_CMD, 0x434D0000u | cmd);
    for (int i = 0; i < 64; i++)
    {
        uint32_t st = cmd_read(HOST_CMD);
        if ((st >> 16) == 0x4F4Bu)
            return (int)(st & 0xFFFFu);
    }
    return -1;
}

/* The request, with the request bit set, and the query the host polls
 * with. hold_until_ack is the request from a running machine. */
static int ask(int is_load)
{
    return host_cmd(is_load ? CMD_LOAD : CMD_SAVE, 1);
}

static int query(int is_load)
{
    return host_cmd(is_load ? CMD_LOAD : CMD_SAVE, 0);
}

static int hold_until_ack(int is_load)
{
    if (!dut->tb_psst_reset_n)
        host_cmd(CMD_RESET_EXIT, 0);
    return ask(is_load) >= 0;
}

/* The core has until the next read strobe to drive bridge_rd_data,
 * because io_bridge_peripheral.v buffers reads by one word. HOST_GAP is
 * about one microsecond of clk_74a, which is shorter than the 88 cycles
 * that io_bridge_peripheral.v gives as the fastest host access. */
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
    return dut->tb_psst_rd_data;
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
    ASSERT_EQ(eng_word(0), blob_read(0));
    ASSERT_EQ(eng_word(0), blob_read(0));
    ASSERT_EQ(eng_word(1), blob_read(1));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_UNDERRUN) != 0);
}

UTEST(psst, it_answers_inside_the_gap)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    for (uint32_t i = 0; i < 40; i++)
        ASSERT_EQ(eng_word(i), blob_read(i));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_UNDERRUN) != 0);
}

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
    /* core_bridge_cmd stays in ST_PARSE until the ack arrives and starts
     * no other host command until then. */
    ASSERT_TRUE(hold_until_ack(0));
    ASSERT_TRUE((int)dut->tb_psst_start_busy);
    ASSERT_FALSE((int)dut->tb_psst_start_ok);
    ASSERT_FALSE((int)dut->tb_psst_start_err);
}

UTEST(psst, a_load_is_acked_without_the_firmware)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    ASSERT_TRUE((int)dut->tb_psst_load_busy);
}

UTEST(psst, a_create_is_answered_without_the_firmware)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    ASSERT_TRUE((int)dut->tb_psst_start_busy);
    ASSERT_FALSE((int)dut->tb_psst_start_ok);
    run(200);
    ASSERT_FALSE((int)dut->tb_psst_start_busy);
    ASSERT_TRUE((int)dut->tb_psst_start_ok);
    ASSERT_FALSE((int)dut->tb_psst_start_err);
    run(400);
    ASSERT_TRUE((int)dut->tb_psst_start_ok);
}

UTEST(psst, the_last_word_gives_the_machine_back)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    ASSERT_TRUE((int)dut->tb_psst_save);
    (void)blob_read(BLOB_WORDS - 2);
    run(16);
    ASSERT_TRUE((int)dut->tb_psst_save);
    (void)blob_read(BLOB_WORDS - 1);
    run(32);
    ASSERT_FALSE((int)dut->tb_psst_save);
}

UTEST(psst, the_two_requests_do_not_collide)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    run(64);
    ASSERT_TRUE((int)dut->tb_psst_load_busy);
    ASSERT_FALSE((int)dut->tb_psst_start_busy);
    ASSERT_FALSE((int)dut->tb_psst_save);
}

UTEST(psst, a_load_goes_to_the_engine_and_the_answer_comes_back)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    run(64);
    ASSERT_TRUE((int)dut->tb_psst_load);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_RESTORED) != 0);

    ASSERT_TRUE((int)dut->tb_psst_load_busy);

    dut->sst_load_done = 1;
    run(16);
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_RESTORED) != 0);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_RESTORE_ERR) != 0);
    ASSERT_TRUE((int)dut->tb_psst_load_ok);
    ASSERT_FALSE((int)dut->tb_psst_load_busy);

    mmio_write(REG_CTL, CTL_RESTORED);
    run(16);
    ASSERT_FALSE((int)dut->tb_psst_load);
    ASSERT_TRUE((int)dut->tb_psst_load_ok);
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
    ASSERT_TRUE((int)dut->tb_psst_load_err);
    ASSERT_FALSE((int)dut->tb_psst_load_ok);
}

UTEST(psst, a_late_answer_is_reported_as_an_error)
{
    reset();
    ASSERT_TRUE(hold_until_ack(0));
    run(200);
    ASSERT_TRUE((int)dut->tb_psst_start_ok);
    dut->bridge_addr = BLOB_BASE;
    dut->bridge_rd = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_rd = 0;
    run(2 * BRG_PERIOD);
    (void)blob_read(1);
    run(32);
    ASSERT_TRUE((int)dut->tb_psst_start_err);
    ASSERT_FALSE((int)dut->tb_psst_start_ok);
}

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
    ASSERT_TRUE((int)dut->tb_psst_start_err);
    (void)blob_read(BLOB_WORDS - 1);
    run(32);
    ASSERT_FALSE((int)dut->tb_psst_save);

    ASSERT_TRUE(hold_until_ack(0));
    ASSERT_FALSE((int)dut->tb_psst_start_ok);
    ASSERT_FALSE((int)dut->tb_psst_start_err);
    ASSERT_TRUE((int)dut->tb_psst_start_busy);
    run(200);
    ASSERT_TRUE((int)dut->tb_psst_start_ok);
    ASSERT_FALSE((int)dut->tb_psst_start_err);
}

UTEST(psst, a_restore_does_not_answer_a_create)
{
    reset();
    ASSERT_TRUE(hold_until_ack(1));
    dut->sst_load_done = 1;
    run(64);
    ASSERT_TRUE((int)dut->tb_psst_load_ok);
    ASSERT_FALSE((int)dut->tb_psst_start_ok);
    ASSERT_FALSE((int)dut->tb_psst_load_busy);
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
    bridge_write(BLOB_BASE - 4);
    bridge_write(BLOB_BASE + BLOB_WINDOW);
    run(64);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_BLOB_SEEN) != 0);
}

/* The result of each command is what the Pocket reads back. */
UTEST(psst, a_save_answers_busy_then_ok)
{
    reset();
    ASSERT_EQ((int)RES_IDLE, query(0));
    ASSERT_EQ((int)RES_BUSY, ask(0));
    ASSERT_EQ((int)RES_BUSY, query(0));
    run(200);
    ASSERT_EQ((int)RES_OK, query(0));
}

UTEST(psst, a_load_into_a_running_machine_answers_busy_then_ok)
{
    reset();
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    ASSERT_EQ((int)RES_IDLE, query(1));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ((int)RES_BUSY, query(1));
    ASSERT_TRUE((int)dut->tb_psst_load);
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_LOAD_EARLY) != 0);
    dut->sst_load_done = 1;
    run(16);
    ASSERT_EQ((int)RES_OK, query(1));
}

UTEST(psst, a_refused_load_into_a_running_machine_answers_err)
{
    reset();
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    dut->sst_load_err = 1;
    dut->sst_load_why = 4;
    dut->sst_load_done = 1;
    run(16);
    ASSERT_EQ((int)RES_ERR, query(1));
    uint32_t ctl = mmio_read(REG_CTL);
    ASSERT_TRUE((ctl & CTL_RESTORE_ERR) != 0);
    ASSERT_EQ(4u, CTL_WHY(ctl));
}

/* The engine cannot run in reset, and the host may wait for the answer
 * before it sends Reset Exit. */
UTEST(psst, a_load_asked_for_in_reset_answers_ok_at_once)
{
    reset();
    ASSERT_FALSE((int)dut->tb_psst_reset_n);
    bridge_write(BLOB_BASE + 0x40);
    ASSERT_EQ((int)RES_OK, ask(1));
    ASSERT_EQ((int)RES_OK, query(1));
    ASSERT_FALSE((int)dut->tb_psst_load_busy);
    run(32);
    /* The request is kept for the engine, and the firmware's first read
     * sees a wake on its way. */
    ASSERT_TRUE((int)dut->tb_psst_load);
    uint32_t ctl = mmio_read(REG_CTL);
    ASSERT_TRUE((ctl & CTL_BLOB_SEEN) != 0);
    ASSERT_TRUE((ctl & CTL_LOAD_EARLY) != 0);
    ASSERT_FALSE((ctl & CTL_BLOB_UNASKED) != 0);

    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    run(64);
    ASSERT_TRUE((int)dut->tb_psst_load);
    ASSERT_EQ((int)RES_OK, query(1));
    dut->sst_load_done = 1;
    run(16);
    ASSERT_EQ((int)RES_OK, query(1));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_BLOB_SEEN) != 0);
    mmio_write(REG_CTL, CTL_RESTORED);
    run(16);
    ASSERT_FALSE((int)dut->tb_psst_load);
    dut->sst_load_done = 0;
    run(16);
    ASSERT_EQ((int)RES_OK, query(1));
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_LOAD_EARLY) != 0);
}

/* The host has its ok and the machine it gets starts its program over,
 * so the answer stays ok. The firmware reads the refusal. */
UTEST(psst, a_load_asked_for_in_reset_and_refused_still_answers_ok)
{
    reset();
    ASSERT_EQ((int)RES_OK, ask(1));
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    dut->sst_load_err = 1;
    dut->sst_load_why = 2;
    dut->sst_load_done = 1;
    run(16);
    ASSERT_EQ((int)RES_OK, query(1));
    uint32_t ctl = mmio_read(REG_CTL);
    ASSERT_TRUE((ctl & CTL_RESTORED) != 0);
    ASSERT_TRUE((ctl & CTL_RESTORE_ERR) != 0);
    ASSERT_TRUE((ctl & CTL_LOAD_EARLY) != 0);
    ASSERT_EQ(2u, CTL_WHY(ctl));
}

/* Ends a load as the engine and the firmware do. */
static void finish_load(int err)
{
    dut->sst_load_err = err;
    dut->sst_load_done = 1;
    run(16);
    mmio_write(REG_CTL, CTL_RESTORED);
    run(8);
    dut->sst_load_done = 0;
    run(16);
}

UTEST(psst, a_request_never_reads_the_result_before_it)
{
    reset();
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    finish_load(1);
    ASSERT_EQ((int)RES_ERR, query(1));
    dut->sst_load_err = 0;
    ASSERT_EQ((int)RES_BUSY, ask(1));
    finish_load(0);
    ASSERT_EQ((int)RES_OK, query(1));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ((int)RES_BUSY, query(1));

    /* A load asked for in reset and then one from the running machine. */
    reset();
    ASSERT_EQ((int)RES_OK, ask(1));
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    finish_load(0);
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_FALSE((mmio_read(REG_CTL) & CTL_LOAD_EARLY) != 0);

    /* A save after a save. */
    reset();
    ASSERT_EQ((int)RES_BUSY, ask(0));
    run(200);
    ASSERT_EQ((int)RES_OK, query(0));
    (void)blob_read(BLOB_WORDS - 1);
    run(32);
    ASSERT_FALSE((int)dut->tb_psst_save);
    ASSERT_EQ((int)RES_OK, query(0));
    ASSERT_EQ((int)RES_BUSY, ask(0));
}

/* The engine's request line is a level the firmware clears, so a second
 * load shows as the level rising again after that clear. */
UTEST(psst, a_repeated_load_request_starts_no_second_load)
{
    reset();
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ(2, (int)dut->tb_psst_loads);
    dut->sst_load_done = 1;
    run(16);
    /* Answered, and the firmware still at work on it. */
    ASSERT_EQ((int)RES_OK, ask(1));
    mmio_write(REG_CTL, CTL_RESTORED);
    run(8);
    ASSERT_FALSE((int)dut->tb_psst_load);
    ASSERT_EQ((int)RES_OK, ask(1));
    run(32);
    ASSERT_FALSE((int)dut->tb_psst_load);
    dut->sst_load_done = 0;
    run(32);
    ASSERT_FALSE((int)dut->tb_psst_load);
    ASSERT_EQ(4, (int)dut->tb_psst_loads);
    /* With that load over, a request is a new load. */
    ASSERT_EQ((int)RES_BUSY, ask(1));
    run(32);
    ASSERT_TRUE((int)dut->tb_psst_load);

    /* In reset as well. */
    reset();
    ASSERT_EQ((int)RES_OK, ask(1));
    ASSERT_EQ((int)RES_OK, ask(1));
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    finish_load(0);
    run(32);
    ASSERT_FALSE((int)dut->tb_psst_load);
}

UTEST(psst, a_repeated_save_request_reads_the_same_blob_again)
{
    reset();
    ASSERT_EQ((int)RES_BUSY, ask(0));
    ASSERT_EQ((int)RES_BUSY, ask(0));
    run(200);
    ASSERT_EQ(eng_word(0), blob_read(0));
    ASSERT_EQ((int)RES_OK, ask(0));
    ASSERT_TRUE((int)dut->tb_psst_save);
    for (uint32_t i = 0; i < 8; i++)
        ASSERT_EQ(eng_word(i), blob_read(i));
    ASSERT_EQ((int)RES_OK, query(0));
}

UTEST(psst, a_blob_with_no_load_asked_for_can_be_dropped)
{
    reset();
    ASSERT_EQ(0, host_cmd(CMD_RESET_EXIT, 0));
    bridge_write(BLOB_BASE + 0x40);
    run(64);
    uint32_t ctl = mmio_read(REG_CTL);
    ASSERT_TRUE((ctl & CTL_BLOB_SEEN) != 0);
    ASSERT_TRUE((ctl & CTL_BLOB_UNASKED) != 0);
    ASSERT_TRUE((ctl & CTL_BLOB_WRITING) != 0);

    /* The mark of a write clears, and the next write sets it again. */
    mmio_write(REG_CTL, CTL_BLOB_WRITING);
    ctl = mmio_read(REG_CTL);
    ASSERT_FALSE((ctl & CTL_BLOB_WRITING) != 0);
    ASSERT_TRUE((ctl & CTL_BLOB_SEEN) != 0);
    bridge_write(BLOB_BASE + 0x44);
    run(64);
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_BLOB_WRITING) != 0);

    mmio_write(REG_CTL, CTL_BLOB_SEEN);
    run(64);
    ctl = mmio_read(REG_CTL);
    ASSERT_FALSE((ctl & CTL_BLOB_SEEN) != 0);
    ASSERT_FALSE((ctl & CTL_BLOB_UNASKED) != 0);
    ASSERT_FALSE((int)dut->tb_psst_load);

    /* A load asked for is itself a wake on its way, and it is not
     * dropped. */
    ASSERT_EQ((int)RES_BUSY, ask(1));
    run(64);
    ctl = mmio_read(REG_CTL);
    ASSERT_TRUE((ctl & CTL_BLOB_SEEN) != 0);
    ASSERT_FALSE((ctl & CTL_BLOB_UNASKED) != 0);
    mmio_write(REG_CTL, CTL_BLOB_SEEN);
    run(64);
    ASSERT_TRUE((mmio_read(REG_CTL) & CTL_BLOB_SEEN) != 0);
    ASSERT_TRUE((int)dut->tb_psst_load);
}

UTEST_MAIN()
