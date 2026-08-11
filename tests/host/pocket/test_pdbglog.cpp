/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The debug log, with the bench playing both the console and the host.
 * Console bytes must arrive four to an event and in order; a short word
 * must go out left-justified once the console falls quiet; a host
 * command must be picked off the bridge and put out whole, ahead of the
 * console and never inside one of its words.
 *
 * The console side plays the firmware, which consults the full flag
 * before every byte, so a burst longer than the queue arrives whole
 * rather than shredded.
 */

#include "Vpocket_dbglog.h"

#include "utest.h"

#include <deque>
#include <string>
#include <vector>

/* Two unrelated periods, near the real 50.4 and 74.25 MHz ratio. */
#define CON_PERIOD 3
#define BRG_PERIOD 2

/* The parameter the module defaults to; the quiet flush waits it out. */
#define FLUSH_TICKS 65536

static Vpocket_dbglog *dut;
static std::vector<uint32_t> events;
static std::deque<uint8_t> console;
static std::deque<std::pair<uint32_t, uint32_t>> writes;
static long tsim;
static int done_hold;
static int prev_event;
/* Bridge edges a command takes to retire. An event is a whole host
 * round trip, so on the device this is orders above the console's own
 * rate; the cases that stay inside the queue do not care what it is. */
static int host_latency;

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
    host_latency = 3;

    dut->clk_mach = 0;
    dut->clk_74a = 0;
    dut->arst_n = 0;
    dut->tx_data = 0;
    dut->tx_valid = 0;
    dut->bridge_wr = 0;
    dut->bridge_endian_little = endian_little;
    dut->bridge_addr = 0;
    dut->bridge_wr_data = 0;
    /* The bridge holds done high between commands. */
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

/* One unit of bench time. Inputs settle before whichever edge is due,
 * the way a registered neighbor would present them. */
static void tick(void)
{
    tsim++;
    bool con_edge = tsim % CON_PERIOD == 0;
    bool brg_edge = tsim % BRG_PERIOD == 0;

    if (con_edge)
    {
        dut->tx_valid = 0;
        /* com_tx_write: look before the store, wait while there is no
         * room. */
        if (!console.empty() && !dut->pocket_dbglog_full)
        {
            dut->tx_data = console.front();
            dut->tx_valid = 1;
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
            /* A zero address is an idle bridge cycle, not a write. */
            if (writes.front().first)
            {
                dut->bridge_addr = writes.front().first;
                dut->bridge_wr_data = writes.front().second;
                dut->bridge_wr = 1;
            }
            writes.pop_front();
        }
        /* Play the bridge: an event is taken, then done falls while the
         * command runs and rises again when it retires. */
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
            done_hold = host_latency;
        }
        prev_event = dut->pocket_dbglog_event;
    }
}

static void run(long units)
{
    for (long i = 0; i < units; i++)
        tick();
}

/* Long enough for anything queued to drain, short of the quiet timer. */
static void settle(void) { run(600); }

/* Long enough for the quiet timer to expire and flush a short word. */
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

/* The command register and its first parameter, as core_bridge_cmd
 * decodes them. The second address byte is a don't care there, so it is
 * a nonzero value here to prove this decode ignores it too. */
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

/* Bridge cycles with nothing on them. The host cannot start a command
 * until the last one has retired, so back-to-back command writes are
 * not a thing it can do, and a burst is spaced. */
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
    /* The hex of the event reads left to right as the text. */
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
    /* Nothing yet: a partial word waits for the quiet period. */
    ASSERT_EQ(0u, (unsigned)events.size());
    settle_quiet();
    ASSERT_EQ(1u, (unsigned)events.size());
    ASSERT_EQ(0x61620000u, events[0]);
}

UTEST(pdbglog, command_goes_out_whole)
{
    reset(0);
    host_command(0x0011, 0); /* Reset Exit: no parameter worth having */
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
    /* The core writes this register back with "ok" and "busy" tags;
     * only "CM" from the host starts a command. */
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
    /* Arrives while the packer holds "ef" and wants two more bytes. */
    run(120);
    host_command(0x008F, 0);
    settle();
    ASSERT_EQ(3u, (unsigned)events.size());
    ASSERT_EQ(0x61626364u, events[0]);
    /* The half-packed word goes first, so the order the console and the
     * host spoke in is the order the log reads in. */
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

/* The case the device is actually in. printf runs at the firmware's
 * speed into a queue drained one word per host round trip, so a restore
 * -- eight lines and a msc report per open descriptor -- outruns 128
 * bytes long before the first word has retired. What is lost is the
 * tail, which is the part that says what went wrong.
 *
 * The claim is the whole line: every byte the console spoke comes out,
 * in order, however far ahead of the host it ran. */
UTEST(pdbglog, a_burst_longer_than_the_queue_loses_nothing)
{
    reset(0);
    host_latency = 40;

    std::string want;
    for (int i = 0; i < 256; i++)
        want.push_back((char)('0' + (i % 64)));
    say(want.c_str());
    run(12000);

    std::string got;
    for (uint32_t e : events)
        for (int b = 24; b >= 0; b -= 8)
            got.push_back((char)((e >> b) & 0xFF));
    ASSERT_STREQ(want.c_str(), got.c_str());
}

UTEST_MAIN()
