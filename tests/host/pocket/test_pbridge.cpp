/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The bridge chain played like the host: an odd-length image streamed
 * as MSB-first words at the host's pacing into SDRAM, the data table
 * answered, completion and reset in the boot order — then byte-exact
 * verification through the staging read port, the release ordered
 * run-then-post, a host re-reset re-posting the length, a reload with
 * a different image, and controller edges leaving as paced HID
 * events.
 *
 * THE HOST PLAYED HERE IS OURS, NOT ANALOGUE'S. core_bridge_cmd.v is
 * published and its wires are therefore known; the order and the
 * pacing of a real load are not documented anywhere and were measured
 * off a Pocket. Expect errors. A core that passes this agrees with our
 * reading of the host, which is not the same as being right.
 */

#include "Vtb_pbridge.h"

#include "utest.h"

#include <vector>

static Vtb_pbridge *dut;
static long g_t;
static long g_sys_edges;
static long a_next, s_next;

/* Advance interleaved clocks to time t: clk_sys period 330, clk_74a
 * period 224 — the true ratio. Records key events as they pass. */
struct KeyEv
{
    int code;
    long at;
};
static std::vector<KeyEv> g_keys;
static std::vector<long> g_slot_sets;
static std::vector<uint32_t> g_slot_lens;
static long g_run_rise = -1, g_run_fall = -1;

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

/* One host word write, then the pacing gap. */
static void host_word(uint32_t addr, uint32_t word, int gap)
{
    dut->bridge_wr = 1;
    dut->bridge_addr = addr;
    dut->bridge_wr_data = word;
    a_cycles(1);
    dut->bridge_wr = 0;
    a_cycles(gap);
}

/* Stream a file image the way the loader does: MSB-first words. */
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

UTEST(pbridge, boot_verify_rereset_reload_keys)
{
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

    /* The boot: stream an odd-length image at the host's pace with a
     * short burst, answer the table, complete, then reset-exit. */
    std::vector<uint8_t> img(1003);
    for (size_t i = 0; i < img.size(); i++)
        img[i] = (uint8_t)(i * 7 + 3);
    dut->datatable_q = (uint32_t)img.size();

    host_stream(img, 75);
    for (int i = 0; i < 4; i++) /* a burst at well under host pace */
        host_word((uint32_t)(992 + i * 4),
                  0xA0A1A2A3u + (uint32_t)i, 40);
    for (size_t i = 992; i < 992 + 16 && i < img.size(); i++)
        img[i] = (uint8_t)((0xA0A1A2A3u + (uint32_t)((i - 992) / 4))
                           >> (24 - 8 * ((i - 992) % 4)));

    ASSERT_EQ((int)dut->tb_pbridge_run, 0);
    /* Completion holds as a level, the bridge command block's way. */
    dut->dataslot_allcomplete = 1;
    a_cycles(20);
    ASSERT_EQ((int)dut->tb_pbridge_run, 0); /* still in host reset */

    dut->reset_n = 1;
    a_cycles(30);
    ASSERT_EQ((int)dut->tb_pbridge_run, 1);

    /* The post follows the rise, never precedes it. Both settle
     * triggers — completion and the reset exit — may post; every post
     * carries the right length. */
    ASSERT_GE((int)g_slot_sets.size(), 1);
    for (size_t i = 0; i < g_slot_sets.size(); i++)
    {
        ASSERT_GT(g_slot_sets[i], g_run_rise);
        ASSERT_EQ(g_slot_lens[i], (uint32_t)img.size());
    }
    size_t posts_boot = g_slot_sets.size();

    /* Byte-exact staging, including the odd tail byte. */
    for (uint32_t h = 0; h * 2 < (uint32_t)img.size(); h++)
    {
        uint16_t want = img[h * 2];
        if (h * 2 + 1 < img.size())
            want |= (uint16_t)img[h * 2 + 1] << 8;
        ASSERT_EQ(stage_read(h) & (h * 2 + 1 < img.size() ? 0xFFFF
                                                          : 0x00FF),
                  want);
    }

    /* A host re-reset re-posts the same length after a fresh rise. */
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

    /* A reload: new image, new length, the full host order again.
     * The next slot request drops the completion level; a button held
     * through the whole reload must deliver itself at the release. */
    dut->reset_n = 0;
    dut->dataslot_allcomplete = 0;
    dut->cont1_key = 1u << 15; /* start, held from here */
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

    /* The pad crosses as state, so the held button simply stands
     * there — no event, nothing to miss, and nothing to deliver late.
     * A release is the absence of the bit, which is the whole reason
     * a pad must not go through the key mailbox. */
    ASSERT_EQ(dut->tb_pbridge_pad_key, 1u << 15);
    dut->cont1_key = 0;
    advance_to(s_next + 330L * 200);
    ASSERT_EQ(dut->tb_pbridge_pad_key, 0u);

    /* Two at once are one word, not two events in some order. The
     * analog words carry across whole, and only when two consecutive
     * samples agree, so an axis never shows half of one poll. */
    dut->cont1_key = (1u << 1) | (1u << 4); /* down + A */
    dut->cont1_joy = 0x20C04080u;
    dut->cont1_trig = 0x1234;
    advance_to(s_next + 330L * 200);
    ASSERT_EQ(dut->tb_pbridge_pad_key, (1u << 1) | (1u << 4));
    ASSERT_EQ(dut->tb_pbridge_pad_joy, 0x20C04080u);
    ASSERT_EQ((int)dut->tb_pbridge_pad_trig, 0x1234);

    /* The dock's keyboard is its own slot and does not touch the pad. */
    dut->cont3_key = 0x40000002u; /* type 4, left-shift held */
    dut->cont3_joy = 0x04050607u;
    dut->cont3_trig = 0x0809;
    advance_to(s_next + 330L * 200);
    ASSERT_EQ(dut->tb_pbridge_kbd_key, 0x40000002u);
    ASSERT_EQ(dut->tb_pbridge_kbd_joy, 0x04050607u);
    ASSERT_EQ((int)dut->tb_pbridge_kbd_trig, 0x0809);
    ASSERT_EQ(dut->tb_pbridge_pad_key, (1u << 1) | (1u << 4));

    dut->cont1_key = 0;
    dut->cont1_joy = 0;
    dut->cont1_trig = 0;
    advance_to(s_next + 330L * 200);

    /* There is no mailbox to fill and nothing to pace against: state
     * that stands is state the firmware reads whenever it looks. */
    dut->cont1_key = 1u << 2; /* left */
    advance_to(s_next + 330L * 500);
    ASSERT_EQ(dut->tb_pbridge_pad_key, 1u << 2);
    dut->cont1_key = 0;
    advance_to(s_next + 330L * 200);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vtb_pbridge;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
