/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vpocket_dbglog.h"

#include "utest.h"

#include <deque>
#include <string>
#include <vector>

/* The 3:2 ratio of these periods is close to the ratio of clk_74a at
 * 74.25 MHz to clk_mach at 50.4 MHz. */
#define CON_PERIOD 3
#define BRG_PERIOD 2

/* This is the default value of pocket_dbglog's FLUSH_TICKS. */
#define FLUSH_TICKS 65536

static Vpocket_dbglog *dut;
static std::vector<uint32_t> events;
static std::deque<uint8_t> console;
static std::deque<std::pair<uint32_t, uint32_t>> writes;
static long tsim;
static int done_hold;
static int prev_event;

static void reset(int endian_little)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vpocket_dbglog;
    events.clear();
    console.clear();
    writes.clear();
    tsim = 0;
    done_hold = 0;
    prev_event = 0;

    dut->clk_mach = 0;
    dut->clk_74a = 0;
    dut->arst_n = 0;
    dut->rv_tx_data = 0;
    dut->rv_tx_valid = 0;
    dut->bridge_wr = 0;
    dut->bridge_endian_little = endian_little;
    dut->bridge_addr = 0;
    dut->bridge_wr_data = 0;
    dut->target_debug_done = 1;
    dut->eval();
    for (int i = 0; i < 8; i++)
    {
        dut->clk_mach = 1;
        dut->clk_74a = 1;
        dut->eval();
        dut->clk_mach = 0;
        dut->clk_74a = 0;
        dut->eval();
    }
    dut->arst_n = 1;
    dut->eval();
}

static void tick(void)
{
    tsim++;
    bool con_edge = tsim % CON_PERIOD == 0;
    bool brg_edge = tsim % BRG_PERIOD == 0;

    if (con_edge)
    {
        dut->rv_tx_valid = 0;
        if (!console.empty())
        {
            dut->rv_tx_data = console.front();
            dut->rv_tx_valid = 1;
            console.pop_front();
        }
        dut->eval();
        dut->clk_mach = 1;
        dut->eval();
        dut->clk_mach = 0;
        dut->eval();
    }

    if (brg_edge)
    {
        dut->bridge_wr = 0;
        if (!writes.empty())
        {
            if (writes.front().first)
            {
                dut->bridge_addr = writes.front().first;
                dut->bridge_wr_data = writes.front().second;
                dut->bridge_wr = 1;
            }
            writes.pop_front();
        }
        if (done_hold > 0 && --done_hold == 0)
            dut->target_debug_done = 1;
        dut->eval();
        dut->clk_74a = 1;
        dut->eval();
        dut->clk_74a = 0;
        dut->eval();

        if (dut->pocket_dbglog_event && !prev_event)
        {
            events.push_back(dut->pocket_dbglog_id);
            dut->target_debug_done = 0;
            done_hold = 3;
        }
        prev_event = dut->pocket_dbglog_event;
    }
}

static void run(long units)
{
    for (long i = 0; i < units; i++)
        tick();
}

/* Six hundred units are enough to drain anything these tests queue, and
 * they end long before FLUSH_TICKS bridge clocks of quiet have passed. */
static void settle(void) { run(600); }

static void settle_quiet(void) { run((long)FLUSH_TICKS * BRG_PERIOD + 2000); }

static void say(const char *s)
{
    for (const char *p = s; *p; p++)
        console.push_back((uint8_t)*p);
}

static uint32_t swap32(uint32_t v)
{
    return ((v & 0xFF) << 24) | ((v & 0xFF00) << 8) | ((v >> 8) & 0xFF00)
           | ((v >> 24) & 0xFF);
}

/* core_bridge_cmd decodes F8xx00xx, so address bits 23:16 are nonzero
 * here to check that pocket_dbglog ignores them too. */
#define HOST_CMD_ADDR 0xF8590000u
#define HOST_PARAM_ADDR 0xF8590020u

static void host_command(uint16_t cmd, int endian_little)
{
    uint32_t w = 0x434D0000u | cmd;
    writes.push_back({HOST_CMD_ADDR, endian_little ? swap32(w) : w});
}

static void host_param(uint32_t p, int endian_little)
{
    writes.push_back({HOST_PARAM_ADDR, endian_little ? swap32(p) : p});
}

/* The host does not start a command until the previous one has
 * finished, so command writes are never back to back. */
static void host_idle(int cycles)
{
    for (int i = 0; i < cycles; i++)
        writes.push_back({0u, 0u});
}

UTEST(pdbglog, console_packs_four_bytes_msb_first)
{
    reset(0);
    say("RP65");
    settle();
    ASSERT_EQ(1u, (unsigned)events.size());
    ASSERT_EQ(0x52503635u, events[0]);
}

UTEST(pdbglog, console_keeps_order_across_words)
{
    reset(0);
    say("abcdefgh");
    settle();
    ASSERT_EQ(2u, (unsigned)events.size());
    ASSERT_EQ(0x61626364u, events[0]);
    ASSERT_EQ(0x65666768u, events[1]);
}

UTEST(pdbglog, short_word_flushes_left_justified)
{
    reset(0);
    say("ab");
    settle();
    ASSERT_EQ(0u, (unsigned)events.size());
    settle_quiet();
    ASSERT_EQ(1u, (unsigned)events.size());
    ASSERT_EQ(0x61620000u, events[0]);
}

UTEST(pdbglog, command_goes_out_whole)
{
    reset(0);
    host_command(0x0011, 0); /* Reset Exit, which takes no parameter */
    settle();
    ASSERT_EQ(1u, (unsigned)events.size());
    ASSERT_EQ(0xC0000011u, events[0]);
}

UTEST(pdbglog, dataslot_command_carries_its_parameter)
{
    reset(0);
    host_param(0x0000000Bu, 0);
    host_command(0x0080, 0); /* Data slot request read, slot 11 */
    settle();
    ASSERT_EQ(2u, (unsigned)events.size());
    ASSERT_EQ(0xC0000080u, events[0]);
    ASSERT_EQ(0x0000000Bu, events[1]);
}

UTEST(pdbglog, savestate_command_carries_its_parameter)
{
    reset(0);
    host_param(0x00000001u, 0);
    host_command(0x00A4, 0); /* Savestate load, request bit set */
    settle();
    ASSERT_EQ(2u, (unsigned)events.size());
    ASSERT_EQ(0xC00000A4u, events[0]);
    ASSERT_EQ(0x00000001u, events[1]);
}

UTEST(pdbglog, other_families_send_no_parameter)
{
    reset(0);
    host_param(0xDEADBEEFu, 0);
    host_command(0x0090, 0); /* Real-time clock */
    settle();
    ASSERT_EQ(1u, (unsigned)events.size());
    ASSERT_EQ(0xC0000090u, events[0]);
}

UTEST(pdbglog, status_write_is_not_a_command)
{
    reset(0);
    /* core_bridge_cmd starts a command only on a word whose top half is
     * "CM", 0x434D, so these "ok" and "BU" words start nothing. */
    writes.push_back({HOST_CMD_ADDR, 0x6F6B0000u});
    writes.push_back({HOST_CMD_ADDR, 0x42550080u});
    settle();
    ASSERT_EQ(0u, (unsigned)events.size());
}

UTEST(pdbglog, endian_little_is_honored)
{
    reset(1);
    host_param(0x00000002u, 1);
    host_command(0x0082, 1);
    settle();
    ASSERT_EQ(2u, (unsigned)events.size());
    ASSERT_EQ(0xC0000082u, events[0]);
    ASSERT_EQ(0x00000002u, events[1]);
}

UTEST(pdbglog, command_never_lands_inside_a_console_word)
{
    reset(0);
    say("abcdef");
    /* After 120 units "ef" is a partial word, so the command arrives in
     * the middle of a word. */
    run(120);
    host_command(0x008F, 0);
    settle();
    ASSERT_EQ(3u, (unsigned)events.size());
    ASSERT_EQ(0x61626364u, events[0]);
    ASSERT_EQ(0x65660000u, events[1]);
    ASSERT_EQ(0xC000008Fu, events[2]);
}

UTEST(pdbglog, a_burst_of_commands_keeps_its_order)
{
    reset(0);
    host_param(0x00000009u, 0);
    host_command(0x0080, 0);
    host_idle(4);
    host_command(0x008F, 0);
    host_idle(4);
    host_command(0x0011, 0);
    settle();
    ASSERT_EQ(4u, (unsigned)events.size());
    ASSERT_EQ(0xC0000080u, events[0]);
    ASSERT_EQ(0x00000009u, events[1]);
    ASSERT_EQ(0xC000008Fu, events[2]);
    ASSERT_EQ(0xC0000011u, events[3]);
}

UTEST_MAIN()
