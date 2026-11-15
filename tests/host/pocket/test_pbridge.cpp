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

static uint16_t stage_read(uint32_t half_addr)
{
    dut->rd_pend = 1;
    dut->rd_addr = half_addr;
    dut->eval();
    int guard = 0;
    while (!dut->tb_pbridge_rvalid && guard++ < 3000)
        advance_to(s_next);
    uint16_t v = dut->tb_pbridge_rdata;
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
    const uint32_t RAM = 0x03EC0000u;
    for (uint32_t i = 0; i < 3; i++)
        host_word(RAM + i * 4, 0xC0DE0000u + i, 1);
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
        ASSERT_EQ(stage_word(RAM + i * 4), 0xC0DE0000u + i);
}

/* The soft CPU's RAM is written by the host only while the machine is in
 * reset. A write there with the machine running never reaches the SDRAM,
 * and the bytes on either side of the RAM are written as ever. */
UTEST(pbridge, a_write_to_the_ram_is_dropped_while_the_machine_runs)
{
    static const uint32_t at[] = {
        0x03EBFFFCu, /* the ROM's last word */
        0x03EC0000u, /* the RAM's first */
        0x03EDFFF0u,
        0x03EFFFFCu, /* the RAM's last */
        0x03F00000u, /* the savestate window's first */
    };
    static const bool ram[] = {false, true, true, true, false};
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
                  (ram[i] ? 0xA0000000u : 0xB0000000u) + (uint32_t)i);

    /* Back in reset the RAM takes writes again, which is a reload. */
    dut->reset_n = 0;
    a_cycles(10);
    ASSERT_EQ((int)dut->tb_pbridge_run, 0);
    for (int i = 0; i < n; i++)
        host_word(at[i], 0xC0000000u + (uint32_t)i, 88);
    a_cycles(60);
    for (int i = 0; i < n; i++)
        ASSERT_EQ(stage_word(at[i]), 0xC0000000u + (uint32_t)i);
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
