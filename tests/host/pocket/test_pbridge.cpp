/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vtb_pbridge.h"

#include "utest.h"

#include <vector>

static Vtb_pbridge *dut;
static long g_t;
static long g_sys_edges;
static long a_next, s_next;

struct KeyEv
{
    int code;
    long at;
};
static std::vector<KeyEv> g_keys;
static std::vector<long> g_slot_sets;
static std::vector<uint32_t> g_slot_lens;
static long g_run_rise = -1, g_run_fall = -1;
/* Set by a clk_sys edge that finds the machine running with a write
 * still in the bridge's queue. */
static bool g_run_queued;
/* Called after every clk_sys edge, to drive the controller's other two
 * clients. */
static void (*g_on_sys)(void);

/* clk_sys has period 330 and clk_74a has period 224, which is the ratio
 * of 74.25 MHz to 50.4 MHz. */
static void advance_to(long t_end)
{
    while (g_t < t_end)
    {
        long next = a_next < s_next ? a_next : s_next;
        if (next > t_end)
        {
            g_t = t_end;
            return;
        }
        g_t = next;
        bool sedge = next == s_next;
        bool aedge = next == a_next;
        int run_before = dut->tb_pbridge_run;
        if (sedge)
            dut->clk_sys = 1;
        if (aedge)
            dut->clk_74a = 1;
        dut->eval();
        if (sedge)
        {
            dut->clk_sys = 0;
            s_next += 330;
            g_sys_edges++;
            if (false)
                g_keys.push_back({0,
                                  g_sys_edges});
            if (dut->tb_pbridge_slot_set)
            {
                g_slot_sets.push_back(g_sys_edges);
                g_slot_lens.push_back(dut->tb_pbridge_slot_len);
            }
            if (dut->tb_pbridge_run && !run_before && g_run_rise < 0)
                g_run_rise = g_sys_edges;
            if (!dut->tb_pbridge_run && run_before)
                g_run_fall = g_sys_edges;
            if (dut->tb_pbridge_run && dut->tb_pbridge_w_queued)
                g_run_queued = true;
            if (g_on_sys)
                g_on_sys();
        }
        if (aedge)
        {
            dut->clk_74a = 0;
            a_next += 224;
        }
        dut->eval();
    }
}

static void a_cycles(int n)
{
    advance_to(a_next + 224L * (n - 1));
}

static void host_word(uint32_t addr, uint32_t word, int gap)
{
    dut->bridge_wr = 1;
    dut->bridge_addr = addr;
    dut->bridge_wr_data = word;
    a_cycles(1);
    dut->bridge_wr = 0;
    a_cycles(gap);
}

static void host_stream(const std::vector<uint8_t> &img, int gap)
{
    for (size_t i = 0; i < img.size(); i += 4)
    {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; k++)
        {
            uint8_t b = i + k < img.size() ? img[i + k] : 0;
            w |= (uint32_t)b << (24 - 8 * k);
        }
        host_word((uint32_t)i, w, gap);
    }
}

/* The read client is asked for a pair of halfwords and answers with
 * both, the one at the even address in the low half. */
static int g_rd_odd;

static void stage_ask(uint32_t half_addr)
{
    dut->rd_pend = 1;
    dut->rd_addr = half_addr >> 1;
    g_rd_odd = half_addr & 1;
}

static uint16_t stage_half(void)
{
    return (uint16_t)(dut->tb_pbridge_rword >> (g_rd_odd * 16));
}

static uint16_t stage_read(uint32_t half_addr)
{
    stage_ask(half_addr);
    dut->eval();
    int guard = 0;
    while (!dut->tb_pbridge_rvalid && guard++ < 3000)
        advance_to(s_next);
    uint16_t v = stage_half();
    dut->rd_pend = 0;
    advance_to(s_next);
    return v;
}

/* A new bridge and SDRAM, held in reset by the host, with the SDRAM
 * past its power-up wait. */
static void power_up(int *utest_result)
{
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vtb_pbridge;
    g_t = g_sys_edges = 0;
    g_keys.clear();
    g_slot_sets.clear();
    g_slot_lens.clear();
    g_run_rise = g_run_fall = -1;
    g_run_queued = false;
    g_on_sys = nullptr;
    a_next = 224;
    s_next = 330;
    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
    for (int i = 0; i < 8; i++)
        advance_to(a_next);
    dut->rst_n = 1;
    dut->arst_n = 1;

    int guard = 0;
    while (!dut->tb_pbridge_ready && guard++ < 60000)
        advance_to(s_next);
    ASSERT_TRUE(dut->tb_pbridge_ready);
}

UTEST(pbridge, boot_verify_rereset_reload_keys)
{
    power_up(utest_result);

    std::vector<uint8_t> img(1003);
    for (size_t i = 0; i < img.size(); i++)
        img[i] = (uint8_t)(i * 7 + 3);
    dut->datatable_q = (uint32_t)img.size();

    host_stream(img, 75);
    for (int i = 0; i < 4; i++)
        host_word((uint32_t)(992 + i * 4),
                  0xA0A1A2A3u + (uint32_t)i, 40);
    for (size_t i = 992; i < 992 + 16 && i < img.size(); i++)
        img[i] = (uint8_t)((0xA0A1A2A3u + (uint32_t)((i - 992) / 4))
                           >> (24 - 8 * ((i - 992) % 4)));

    ASSERT_EQ((int)dut->tb_pbridge_run, 0);
    dut->dataslot_allcomplete = 1;
    a_cycles(20);
    ASSERT_EQ((int)dut->tb_pbridge_run, 0);

    dut->reset_n = 1;
    a_cycles(30);
    ASSERT_EQ((int)dut->tb_pbridge_run, 1);

    ASSERT_GE((int)g_slot_sets.size(), 1);
    for (size_t i = 0; i < g_slot_sets.size(); i++)
    {
        ASSERT_GT(g_slot_sets[i], g_run_rise);
        ASSERT_EQ(g_slot_lens[i], (uint32_t)img.size());
    }
    size_t posts_boot = g_slot_sets.size();

    for (uint32_t h = 0; h * 2 < (uint32_t)img.size(); h++)
    {
        uint16_t want = img[h * 2];
        if (h * 2 + 1 < img.size())
            want |= (uint16_t)img[h * 2 + 1] << 8;
        ASSERT_EQ(stage_read(h) & (h * 2 + 1 < img.size() ? 0xFFFF
                                                          : 0x00FF),
                  want);
    }

    dut->reset_n = 0;
    a_cycles(10);
    ASSERT_EQ((int)dut->tb_pbridge_run, 0);
    ASSERT_GT(g_run_fall, 0L);
    dut->reset_n = 1;
    a_cycles(30);
    ASSERT_EQ((int)dut->tb_pbridge_run, 1);
    ASSERT_GT(g_slot_sets.size(), posts_boot);
    for (size_t i = posts_boot; i < g_slot_sets.size(); i++)
        ASSERT_EQ(g_slot_lens[i], (uint32_t)img.size());
    size_t posts_rereset = g_slot_sets.size();

    dut->reset_n = 0;
    dut->dataslot_allcomplete = 0;
    dut->cont1_key = 1u << 15; /* start */
    a_cycles(10);
    std::vector<uint8_t> img2(400);
    for (size_t i = 0; i < img2.size(); i++)
        img2[i] = (uint8_t)(200 - i);
    dut->datatable_q = (uint32_t)img2.size();
    host_stream(img2, 88);
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
    a_cycles(30);
    ASSERT_EQ((int)dut->tb_pbridge_run, 1);
    ASSERT_GT(g_slot_sets.size(), posts_rereset);
    ASSERT_EQ(g_slot_lens.back(), (uint32_t)img2.size());
    for (uint32_t h = 0; h < 200; h += 7)
        ASSERT_EQ(stage_read(h),
                  (uint16_t)(img2[h * 2] | (img2[h * 2 + 1] << 8)));

    ASSERT_EQ(dut->tb_pbridge_cont1_key, 1u << 15);
    dut->cont1_key = 0;
    advance_to(s_next + 330L * 200);
    ASSERT_EQ(dut->tb_pbridge_cont1_key, 0u);

    dut->cont1_key = (1u << 1) | (1u << 4); /* down + A */
    dut->cont1_joy = 0x20C04080u;
    dut->cont1_trig = 0x1234;
    advance_to(s_next + 330L * 200);
    ASSERT_EQ(dut->tb_pbridge_cont1_key, (1u << 1) | (1u << 4));
    ASSERT_EQ(dut->tb_pbridge_cont1_joy, 0x20C04080u);
    ASSERT_EQ((int)dut->tb_pbridge_cont1_trig, 0x1234);

    dut->cont2_key = 0x3000000Au; /* type 3, an analog gamepad, down + right */
    dut->cont2_joy = 0x11223344u;
    dut->cont2_trig = 0x5566;
    dut->cont3_key = 0x40000002u; /* type 4, a keyboard */
    dut->cont3_joy = 0x04050607u;
    dut->cont3_trig = 0x0809;
    dut->cont4_key = 0x50000007u; /* type 5, a mouse */
    dut->cont4_joy = 0x0001FFFEu;
    dut->cont4_trig = 0x0003;
    advance_to(s_next + 330L * 200);
    ASSERT_EQ(dut->tb_pbridge_cont2_key, 0x3000000Au);
    ASSERT_EQ(dut->tb_pbridge_cont2_joy, 0x11223344u);
    ASSERT_EQ((int)dut->tb_pbridge_cont2_trig, 0x5566);
    ASSERT_EQ(dut->tb_pbridge_cont3_key, 0x40000002u);
    ASSERT_EQ(dut->tb_pbridge_cont3_joy, 0x04050607u);
    ASSERT_EQ((int)dut->tb_pbridge_cont3_trig, 0x0809);
    ASSERT_EQ(dut->tb_pbridge_cont4_key, 0x50000007u);
    ASSERT_EQ(dut->tb_pbridge_cont4_joy, 0x0001FFFEu);
    ASSERT_EQ((int)dut->tb_pbridge_cont4_trig, 0x0003);
    ASSERT_EQ(dut->tb_pbridge_cont1_key, (1u << 1) | (1u << 4));

    dut->cont1_key = 0;
    dut->cont1_joy = 0;
    dut->cont1_trig = 0;
    advance_to(s_next + 330L * 200);

    dut->cont1_key = 1u << 2; /* left */
    advance_to(s_next + 330L * 500);
    ASSERT_EQ(dut->tb_pbridge_cont1_key, 1u << 2);
    dut->cont1_key = 0;
    advance_to(s_next + 330L * 200);
}

/* The four halves of the words a test writes at one address. */
/* A new ROM chosen from the menu while the machine runs: the host asks to
 * write slot 0, writes the image, then sends 0x008F. The bridge says a ROM
 * is coming from the request until the new size is posted. A request for
 * another slot, or one made in reset, is no new ROM. */
static void request_write(uint16_t id)
{
    dut->dataslot_requestwrite_id = id;
    dut->dataslot_requestwrite = 1;
    a_cycles(2);
    dut->dataslot_requestwrite = 0;
    a_cycles(10);
}

UTEST(pbridge, a_new_rom_stops_the_machine_until_it_has_landed)
{
    power_up(utest_result);
    std::vector<uint8_t> img(64, 0x11);
    dut->datatable_q = (uint32_t)img.size();
    request_write(0);
    ASSERT_EQ((int)dut->tb_pbridge_rom_coming, 0);
    host_stream(img, 40);
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
    a_cycles(30);
    ASSERT_EQ((int)dut->tb_pbridge_run, 1);
    ASSERT_EQ((int)dut->tb_pbridge_rom_coming, 0);

    request_write(1);
    ASSERT_EQ((int)dut->tb_pbridge_rom_coming, 0);

    dut->dataslot_allcomplete = 0;
    request_write(0);
    ASSERT_EQ((int)dut->tb_pbridge_rom_coming, 1);
    std::vector<uint8_t> img2(80, 0x22);
    dut->datatable_q = (uint32_t)img2.size();
    host_stream(img2, 40);
    ASSERT_EQ((int)dut->tb_pbridge_rom_coming, 1);
    size_t posts = g_slot_sets.size();
    dut->dataslot_allcomplete = 1;
    a_cycles(30);
    ASSERT_GT(g_slot_sets.size(), posts);
    ASSERT_EQ(g_slot_lens.back(), (uint32_t)img2.size());
    ASSERT_EQ((int)dut->tb_pbridge_rom_coming, 0);
}

static uint32_t stage_word(uint32_t addr)
{
    uint16_t lo = stage_read(addr >> 1);
    uint16_t hi = stage_read((addr >> 1) + 1);
    return (uint32_t)(lo & 0xFF) << 24 | (uint32_t)(lo >> 8) << 16
           | (uint32_t)(hi & 0xFF) << 8 | (uint32_t)(hi >> 8);
}

/* The host's Reset Exit comes on the heels of its last writes here, with
 * six of them still queued behind an SDRAM that was in self refresh. The
 * machine must stay in reset until the queue is empty, or the soft CPU
 * could fetch firmware that has not landed. */
UTEST(pbridge, the_machine_waits_for_the_write_queue)
{
    power_up(utest_result);
    dut->datatable_q = 12;
    dut->dataslot_allcomplete = 1;
    a_cycles(40);
    ASSERT_EQ((int)dut->tb_pbridge_run, 0);
    ASSERT_EQ((int)dut->tb_pbridge_w_queued, 0);

    /* 400 idle clocks puts the SDRAM in self refresh. */
    advance_to(s_next + 330L * 400);
    const uint32_t MEM = 0x03E00000u;
    for (uint32_t i = 0; i < 3; i++)
        host_word(MEM + i * 4, 0xC0DE0000u + i, 1);
    dut->reset_n = 1;

    long queued_edges = 0;
    int guard = 0;
    while (!dut->tb_pbridge_run && guard++ < 2000)
    {
        advance_to(s_next);
        if (dut->tb_pbridge_w_queued)
            queued_edges++;
    }
    ASSERT_EQ((int)dut->tb_pbridge_run, 1);
    /* The wait was a real one: the queue was seen holding writes after
     * Reset Exit, for longer than the reset's own way through the
     * synchronizers. */
    ASSERT_GT(queued_edges, 8L);
    advance_to(s_next + 330L * 200);
    ASSERT_FALSE(g_run_queued);
    for (uint32_t i = 0; i < 3; i++)
        ASSERT_EQ(stage_word(MEM + i * 4), 0xC0DE0000u + i);
}

/* The soft CPU's ROM and RAM blocks are written by the host only while
 * the machine is in reset. A write there with the machine running never
 * reaches the SDRAM, and the bytes on either side of them, the first file
 * window's included, are written as ever. */
UTEST(pbridge, a_write_to_the_firmware_is_dropped_while_the_machine_runs)
{
    static const uint32_t at[] = {
        0x03DFFFFCu, /* the ROM slot's last word */
        0x03E00000u, /* the ROM block's first */
        0x03E3FFFCu, /* the ROM block's last */
        0x03E40000u, /* the RAM block's first */
        0x03E4FFFCu, /* the RAM block's last */
        0x03E80000u, /* the first file window's first */
    };
    static const bool mem[] = {false, true, true, true, true, false};
    const int n = (int)(sizeof at / sizeof *at);

    power_up(utest_result);
    for (int i = 0; i < n; i++)
        host_word(at[i], 0xA0000000u + (uint32_t)i, 88);
    dut->datatable_q = 4;
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
    a_cycles(60);
    ASSERT_EQ((int)dut->tb_pbridge_run, 1);
    for (int i = 0; i < n; i++)
        ASSERT_EQ(stage_word(at[i]), 0xA0000000u + (uint32_t)i);

    for (int i = 0; i < n; i++)
        host_word(at[i], 0xB0000000u + (uint32_t)i, 88);
    a_cycles(60);
    ASSERT_EQ((int)dut->tb_pbridge_w_queued, 0);
    for (int i = 0; i < n; i++)
        ASSERT_EQ(stage_word(at[i]),
                  (mem[i] ? 0xA0000000u : 0xB0000000u) + (uint32_t)i);

    /* Back in reset the firmware takes writes again, which is a reload. */
    dut->reset_n = 0;
    a_cycles(10);
    ASSERT_EQ((int)dut->tb_pbridge_run, 0);
    for (int i = 0; i < n; i++)
        host_word(at[i], 0xC0000000u + (uint32_t)i, 88);
    a_cycles(60);
    for (int i = 0; i < n; i++)
        ASSERT_EQ(stage_word(at[i]), 0xC0000000u + (uint32_t)i);
}

/* The firmware reads a file window, the host fills the window again, and
 * the firmware reads once the write queue is empty. The line that the
 * first read left held is gone by then, wherever the host wrote, so no
 * read is answered from before the write. */
UTEST(pbridge, a_host_write_throws_the_held_line_away)
{
    power_up(utest_result);
    const uint32_t WIN = 0x03E80000u;
    for (uint32_t i = 0; i < 12; i++)
        host_word(WIN + i * 4, 0x11110000u + i, 88);
    a_cycles(60);

    /* The held line is the window's second. The host writes into it,
     * into the line on each side, and far away. */
    const uint32_t targets[] = {WIN + 20, WIN + 12, WIN + 32, 0x00100000u};
    uint32_t n = 0;
    for (uint32_t t : targets)
    {
        ASSERT_EQ(stage_word(WIN + 16), 0x11110004u);
        stage_ask((WIN + 24) >> 1);
        dut->eval();
        ASSERT_TRUE(dut->tb_pbridge_rvalid);
        dut->rd_pend = 0;

        host_word(t, 0x22220000u + n, 8);
        int guard = 0;
        while (dut->tb_pbridge_w_queued && guard++ < 3000)
            advance_to(s_next);
        ASSERT_EQ((int)dut->tb_pbridge_w_queued, 0);

        stage_ask((WIN + 24) >> 1);
        dut->eval();
        ASSERT_FALSE(dut->tb_pbridge_rvalid);
        dut->rd_pend = 0;
        ASSERT_EQ(stage_word(t), 0x22220000u + n);
        ASSERT_EQ(stage_word(WIN + 24), 0x11110006u);
        n++;
    }
}

/* The soft CPU's side of the flood. It has one access in flight, a stage
 * read or a line, and asks for the next the clock the last is answered,
 * each in a row that is not open. It asks for the same kind for a few
 * hundred clocks at a time, as a file read or cold code does. Reads are
 * of words the host wrote a while ago and lines are in a part of the
 * SDRAM the host leaves alone. */
#include <map>

struct flood_t
{
    std::vector<uint32_t> at, word;
    std::map<uint32_t, std::vector<uint16_t>> lines;
    uint32_t rng = 0x2545F491u;
    long reads = 0, line_reads = 0, line_writes = 0;
    long bad_reads = 0, bad_lines = 0;
    bool rd_known = false;
    int run = 0;
    bool run_lines = false;
    uint16_t rd_want = 0;
    std::vector<uint16_t> line_out;
};
static flood_t g_flood;

static uint32_t flood_rnd(void)
{
    uint32_t &x = g_flood.rng;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* The halfword of the SDRAM that holds a host word's first or second two
 * bytes, the even byte low. */
static uint16_t host_half(uint32_t word, int second)
{
    uint32_t two = second ? word & 0xFFFFu : word >> 16;
    return (uint16_t)((two >> 8) | (two << 8));
}

static void flood_sys(void)
{
    flood_t &f = g_flood;
    if (dut->rd_pend && dut->tb_pbridge_rvalid)
    {
        f.reads++;
        if (f.rd_known && stage_half() != f.rd_want)
            f.bad_reads++;
        dut->rd_pend = 0;
    }
    if (dut->line_pend && dut->tb_pbridge_line_done)
    {
        std::vector<uint16_t> got(8);
        for (int k = 0; k < 8; k++)
            got[k] = (uint16_t)(dut->tb_pbridge_line_rdata[k / 2]
                                >> ((k & 1) * 16));
        if (dut->line_we)
        {
            f.line_writes++;
            f.lines[dut->line_addr] = f.line_out;
        }
        else
        {
            f.line_reads++;
            if (f.lines[dut->line_addr] != got)
                f.bad_lines++;
        }
        dut->line_pend = 0;
    }
    if (dut->rd_pend || dut->line_pend)
        return;
    if (f.run-- == 0)
    {
        f.run = 16 + (int)(flood_rnd() % 48);
        f.run_lines = !f.run_lines;
    }
    if (dut->tb_pbridge_line_done || !f.run_lines)
    {
        /* The last 64 words the host sent may still be on their way. */
        f.rd_known = f.at.size() > 64;
        uint32_t i = f.rd_known ? flood_rnd() % (f.at.size() - 64) : 0;
        int second = flood_rnd() & 1;
        stage_ask(f.rd_known ? (f.at[i] >> 1) + second : 0);
        f.rd_want = f.rd_known ? host_half(f.word[i], second) : 0;
    }
    else
    {
        /* Half are reads of a line written before, once there is one. */
        bool we = f.lines.empty() || (flood_rnd() & 1);
        /* A line of the ROM or the RAM block. */
        uint32_t line = 0x3E0000u + flood_rnd() % 0x5000u;
        if (!we)
        {
            auto it = f.lines.begin();
            std::advance(it, flood_rnd() % f.lines.size());
            line = it->first;
        }
        f.line_out.resize(8);
        for (int k = 0; k < 8; k++)
        {
            f.line_out[k] = (uint16_t)flood_rnd();
            uint32_t &w = dut->line_wdata[k / 2];
            int shift = (k & 1) * 16;
            w = (w & ~(0xFFFFu << shift)) | ((uint32_t)f.line_out[k] << shift);
        }
        dut->line_pend = 1;
        dut->line_we = we;
        dut->line_addr = line;
    }
}

/* The host writes as fast as it can, a word every 88 clk_74a cycles, to a
 * new row every time, while reads and lines are asked for back to back.
 * The bridge's queue holds eight halfwords and cannot make the host wait,
 * and it stops the simulation if it overflows. The controller takes a
 * write before a read or a line, so every word lands, and the reads and
 * the lines still get their turns and their data. */
UTEST(pbridge, a_flood_of_writes_loses_nothing_to_reads_and_lines)
{
    const uint32_t WORDS = 6000;
    power_up(utest_result);
    g_flood = flood_t();
    flood_t &f = g_flood;
    g_on_sys = flood_sys;
    for (uint32_t i = 0; f.at.size() < WORDS; i++)
    {
        /* Odd times i covers every word address once, in a scattered
         * order. The soft CPU's ROM and RAM blocks, which the bridge
         * drops while the machine runs, are left out. */
        uint32_t at = ((i * 0x9E3779B1u) & 0xFFFFFFu) << 2;
        if (at >= 0x03E00000u && at < 0x03E50000u)
            continue;
        uint32_t word = flood_rnd();
        host_word(at, word, 87);
        f.at.push_back(at);
        f.word.push_back(word);
    }
    a_cycles(400);
    g_on_sys = nullptr;
    dut->rd_pend = 0;
    dut->line_pend = 0;
    advance_to(s_next + 330L * 40);
    ASSERT_EQ((int)dut->tb_pbridge_w_queued, 0);

    fprintf(stderr, "  %u host words, %ld reads, %ld line reads, "
                    "%ld line writes\n",
            WORDS, f.reads, f.line_reads, f.line_writes);
    ASSERT_EQ(0L, f.bad_reads);
    ASSERT_EQ(0L, f.bad_lines);
    /* A host word is 60 clk_sys cycles, room for a read or a line or
     * two beside its two writes. */
    ASSERT_GT(f.reads, (long)WORDS / 2);
    ASSERT_GT(f.line_reads + f.line_writes, (long)WORDS / 2);
    for (uint32_t i = 0; i < WORDS; i++)
    {
        ASSERT_EQ(stage_read(f.at[i] >> 1), host_half(f.word[i], 0));
        ASSERT_EQ(stage_read((f.at[i] >> 1) + 1), host_half(f.word[i], 1));
    }
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
