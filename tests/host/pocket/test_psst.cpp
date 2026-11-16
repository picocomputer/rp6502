/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The host here, and where each piece comes from:
 * - The commands, their result codes and the window the host reads
 *   after ok: Analogue's docs,
 *   https://www.analogue.co/developer/docs/openfpga/host-target-commands
 * - Commands as writes to 0xF8000000 and a poll of the result, and reads
 *   buffered one word, one every 88 clk_74a cycles at the fastest:
 *   Analogue's template, core_bridge_cmd.v and io_bridge_peripheral.v in
 *   vendor/openfpga, https://github.com/open-fpga/core-template
 * - A load asked for in reset, then Reset Exit: a community PlayStation
 *   core reports loads in reset, and the Spacewar core,
 *   https://github.com/spacemen3/PDP-1, answers them there.
 * - The host waiting for the load's answer before Reset Exit, and a
 *   request repeated while one is with the firmware: our guess.
 */

#include "Vtb_psst.h"
#include "Vtb_psst___024root.h"
#include "pocket_version.h"

#include "utest.h"

#include <vector>

/* clk_74a pulses every second unit and clk_sys every third, a ratio of
 * 1.5, where the board runs them at 74.25 MHz and 50.4 MHz. */
#define BRG_PERIOD 2
#define SYS_PERIOD 3

#define BLOB_BASE 0x03EC1000u
#define BLOB_WINDOW 0x0013F000u

#define REG_CTL 0x00u
#define CTL_SAVE_REQ (1u << 0)
#define CTL_LOAD_REQ (1u << 1)
#define CTL_BLOB_SEEN (1u << 2)
#define CTL_WQ_EMPTY (1u << 3)
#define SAVE_OK 1u
#define SAVE_ERR 2u
#define LOAD_OK 3u
#define LOAD_ERR 4u

/* core_bridge_cmd's host command register, its first parameter, its
 * third response word, the commands, and the result codes of the two
 * savestate commands. */
#define HOST_CMD 0xF8000000u
#define HOST_PARAM 0xF8000020u
#define HOST_48 0xF8000048u
#define CMD_RESET_ENTER 0x0010u
#define CMD_RESET_EXIT 0x0011u
#define CMD_SAVE 0x00A0u
#define CMD_LOAD 0x00A4u
#define RES_IDLE 0u
#define RES_BUSY 1u
#define RES_OK 2u
#define RES_ERR 3u

static Vtb_psst *dut;
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

/* The byte the window holds at offset i, and the word the host reads at
 * word index k, its first byte in bits 31:24. */
static uint8_t blob_byte(uint32_t i)
{
    return (uint8_t)(i * 7u + (i >> 8) * 13u + 0x5Au);
}

static uint32_t blob_word(uint32_t k)
{
    uint32_t w = 0;
    for (uint32_t b = 0; b < 4; b++)
        w = (w << 8) | blob_byte(k * 4 + b);
    return w;
}

/* Fills the chip model's window, two bytes a word with the even byte
 * low, as the bridge and the soft CPU's cache both write it. */
static void fill_window(uint32_t bytes)
{
    for (uint32_t i = 0; i < bytes; i += 2)
        dut->rootp->tb_psst__DOT__chip__DOT__mem[(BLOB_BASE + i) >> 1] =
            (uint16_t)(blob_byte(i) | blob_byte(i + 1) << 8);
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
    dut->w_queued = 0;
    dut->bridge_wr = 0;
    dut->bridge_addr = 0;
    dut->bridge_wr_data = 0;
    dut->bridge_rd = 0;
    dut->eval();
    run(16);
    dut->arst_n = 1;
    for (long i = 0; i < 100000 && !dut->tb_psst_sdram_ready; i++)
        tick();
    fill_window(4096);
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

static uint32_t ctl(void) { return mmio_read(REG_CTL); }

/* The firmware's answer, and long enough for it to cross. */
static void answer(uint32_t code)
{
    mmio_write(REG_CTL, code);
    run(32);
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
 * with. */
static int ask(int is_load)
{
    return host_cmd(is_load ? CMD_LOAD : CMD_SAVE, 1);
}

static int query(int is_load)
{
    return host_cmd(is_load ? CMD_LOAD : CMD_SAVE, 0);
}

static void reset_exit(void) { host_cmd(CMD_RESET_EXIT, 0); }

/* The core has until the next read strobe to drive bridge_rd_data,
 * because io_bridge_peripheral.v buffers reads by one word. HOST_GAP is
 * about one microsecond of clk_74a, which is shorter than the 88 cycles
 * that io_bridge_peripheral.v gives as the fastest host access. */
#define HOST_GAP 74

static void blob_strobe(uint32_t idx, long gap)
{
    dut->bridge_addr = BLOB_BASE + idx * 4;
    dut->bridge_rd = 1;
    do
        tick();
    while (t % BRG_PERIOD != 0);
    dut->bridge_rd = 0;
    run(gap * BRG_PERIOD);
}

static uint32_t blob_read(uint32_t idx)
{
    blob_strobe(idx, HOST_GAP);
    return dut->tb_psst_rd_data;
}

/* A save the firmware has answered ok. */
static void saved(void)
{
    reset_exit();
    ask(0);
    run(64);
    answer(SAVE_OK);
}

UTEST(psst, a_save_waits_for_the_firmware_and_then_reads_the_sdram)
{
    reset();
    reset_exit();
    ASSERT_EQ((int)RES_IDLE, query(0));
    ASSERT_EQ(0u, ctl() & CTL_SAVE_REQ);
    ASSERT_EQ((int)RES_BUSY, ask(0));
    run(2000);
    ASSERT_EQ((int)RES_BUSY, query(0));
    ASSERT_NE(0u, ctl() & CTL_SAVE_REQ);
    answer(SAVE_OK);
    ASSERT_EQ(0u, ctl() & CTL_SAVE_REQ);
    ASSERT_EQ((int)RES_OK, query(0));
    for (uint32_t i = 0; i < 300; i++)
        ASSERT_EQ(blob_word(i), blob_read(i));
    ASSERT_EQ((int)RES_OK, query(0));
}

UTEST(psst, the_size_answered_is_sst_bytes)
{
    reset();
    reset_exit();
    ASSERT_EQ((int)RES_IDLE, query(0));
    ASSERT_EQ((uint32_t)POCKET_SST_BYTES, cmd_read(HOST_48));
    ASSERT_EQ((int)RES_IDLE, query(1));
    ASSERT_EQ(BLOB_WINDOW, cmd_read(HOST_48));
}

UTEST(psst, any_address_and_a_repeated_one_are_answered)
{
    reset();
    saved();
    ASSERT_EQ(blob_word(900), blob_read(900));
    ASSERT_EQ(blob_word(3), blob_read(3));
    ASSERT_EQ(blob_word(3), blob_read(3));
    ASSERT_EQ(blob_word(1000), blob_read(1000));
    ASSERT_EQ(blob_word(4), blob_read(4));
    ASSERT_EQ((int)RES_OK, query(0));
}

/* A strobe that comes before the word asked for is fetched finds it not
 * sent, and the save answers err from then on. */
UTEST(psst, a_slow_fetch_answers_err)
{
    reset();
    saved();
    ASSERT_EQ(blob_word(0), blob_read(0));
    blob_strobe(4, 2);
    (void)blob_read(5);
    ASSERT_EQ((int)RES_ERR, query(0));
    /* The next save starts clean. */
    ASSERT_EQ((int)RES_BUSY, ask(0));
    answer(SAVE_OK);
    ASSERT_EQ(blob_word(7), blob_read(7));
    ASSERT_EQ((int)RES_OK, query(0));
}

UTEST(psst, a_save_the_firmware_refuses_answers_err)
{
    reset();
    reset_exit();
    ASSERT_EQ((int)RES_BUSY, ask(0));
    run(64);
    answer(SAVE_ERR);
    ASSERT_EQ((int)RES_ERR, query(0));
    ASSERT_EQ(0u, ctl() & CTL_SAVE_REQ);
    /* An answer with no request waiting changes nothing. */
    answer(SAVE_OK);
    ASSERT_EQ((int)RES_ERR, query(0));
    ASSERT_EQ((int)RES_BUSY, ask(0));
    answer(SAVE_OK);
    ASSERT_EQ((int)RES_OK, query(0));
}

/* A request while a save is with the firmware starts nothing; one after
 * its answer is a new save. */
UTEST(psst, a_repeated_save_request_starts_no_second_save)
{
    reset();
    reset_exit();
    ASSERT_EQ((int)RES_BUSY, ask(0));
    ASSERT_EQ((int)RES_BUSY, ask(0));
    ASSERT_EQ(2, (int)dut->tb_psst_starts);
    answer(SAVE_OK);
    ASSERT_EQ(0u, ctl() & CTL_SAVE_REQ);
    ASSERT_EQ((int)RES_OK, query(0));
    ASSERT_EQ((int)RES_BUSY, ask(0));
    ASSERT_NE(0u, ctl() & CTL_SAVE_REQ);
}

UTEST(psst, a_load_into_a_running_machine_waits_for_the_firmware)
{
    reset();
    reset_exit();
    ASSERT_EQ((int)RES_IDLE, query(1));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    run(2000);
    ASSERT_EQ((int)RES_BUSY, query(1));
    uint32_t c = ctl();
    ASSERT_NE(0u, c & CTL_LOAD_REQ);
    ASSERT_NE(0u, c & CTL_BLOB_SEEN);
    answer(LOAD_OK);
    c = ctl();
    ASSERT_EQ(0u, c & CTL_LOAD_REQ);
    ASSERT_EQ(0u, c & CTL_BLOB_SEEN);
    ASSERT_EQ((int)RES_OK, query(1));

    ASSERT_EQ((int)RES_BUSY, ask(1));
    answer(LOAD_ERR);
    ASSERT_EQ((int)RES_ERR, query(1));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ((int)RES_BUSY, query(1));
}

/* The host may wait for the answer before it sends Reset Exit, and the
 * firmware does not run in reset. The request is kept for it. */
UTEST(psst, a_load_asked_for_in_reset_answers_ok_at_once)
{
    reset();
    ASSERT_FALSE((int)dut->tb_psst_reset_n);
    bridge_write(BLOB_BASE + 0x40);
    ASSERT_EQ((int)RES_OK, ask(1));
    ASSERT_EQ((int)RES_OK, query(1));
    ASSERT_FALSE((int)dut->tb_psst_load_busy);
    run(32);
    uint32_t c = ctl();
    ASSERT_NE(0u, c & CTL_LOAD_REQ);
    ASSERT_NE(0u, c & CTL_BLOB_SEEN);

    reset_exit();
    run(64);
    ASSERT_NE(0u, ctl() & CTL_LOAD_REQ);
    ASSERT_EQ((int)RES_OK, query(1));
    /* The host has its ok, so a refusal leaves it. */
    answer(LOAD_ERR);
    ASSERT_EQ((int)RES_OK, query(1));
    c = ctl();
    ASSERT_EQ(0u, c & CTL_LOAD_REQ);
    ASSERT_EQ(0u, c & CTL_BLOB_SEEN);
    /* A load from the running machine after it is its own. */
    ASSERT_EQ((int)RES_BUSY, ask(1));
}

/* While a load is with the firmware a request is acknowledged and
 * answered with that load's status. */
UTEST(psst, a_repeated_load_request_starts_no_second_load)
{
    reset();
    reset_exit();
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ(2, (int)dut->tb_psst_loads);
    answer(LOAD_OK);
    ASSERT_EQ(0u, ctl() & CTL_LOAD_REQ);
    /* The answer was the first load's and nothing is left waiting. */
    ASSERT_EQ((int)RES_OK, query(1));
    run(64);
    ASSERT_EQ(0u, ctl() & CTL_LOAD_REQ);
}

UTEST(psst, a_write_into_the_window_is_a_blob_arriving)
{
    reset();
    ASSERT_EQ(0u, ctl() & CTL_BLOB_SEEN);
    bridge_write(BLOB_BASE - 4);
    bridge_write(BLOB_BASE + BLOB_WINDOW);
    run(64);
    ASSERT_EQ(0u, ctl() & CTL_BLOB_SEEN);
    bridge_write(BLOB_BASE + 0x40);
    run(64);
    ASSERT_NE(0u, ctl() & CTL_BLOB_SEEN);
}

UTEST(psst, the_write_queue_shows_in_the_control_register)
{
    reset();
    ASSERT_NE(0u, ctl() & CTL_WQ_EMPTY);
    dut->w_queued = 1;
    ASSERT_EQ(0u, ctl() & CTL_WQ_EMPTY);
    dut->w_queued = 0;
    ASSERT_NE(0u, ctl() & CTL_WQ_EMPTY);
}

/* Saves and loads interleave without one taking the other's answer. */
UTEST(psst, the_two_requests_do_not_collide)
{
    reset();
    reset_exit();
    ASSERT_EQ((int)RES_BUSY, ask(1));
    ASSERT_EQ((int)RES_BUSY, ask(0));
    uint32_t c = ctl();
    ASSERT_NE(0u, c & CTL_SAVE_REQ);
    ASSERT_NE(0u, c & CTL_LOAD_REQ);
    answer(SAVE_ERR);
    ASSERT_EQ((int)RES_ERR, query(0));
    ASSERT_EQ((int)RES_BUSY, query(1));
    answer(LOAD_OK);
    ASSERT_EQ((int)RES_OK, query(1));
    ASSERT_EQ((int)RES_ERR, query(0));
}

UTEST_MAIN()
