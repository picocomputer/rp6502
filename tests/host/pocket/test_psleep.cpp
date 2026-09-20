/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static Vtb_pocket *dut;
static long a_next, s_next, g_sys;
static uint32_t dt_pipe[2];
static uint32_t g_dt[64];

static std::map<std::string, std::vector<uint8_t>> g_files;
static std::set<std::string> g_dirs;
static std::string g_bound[16];
static std::string g_console;
static std::string g_rv;
static int g_opens, g_reads, g_writes, g_flushes, g_getfiles;

enum { REQ_NONE = 0, REQ_OPEN, REQ_FLUSH, REQ_GETFILE, REQ_READ, REQ_WRITE };
static int g_req, g_servicing;
static int g_prev_r, g_prev_w, g_prev_o, g_prev_f, g_prev_g;

static void tick()
{
    long next = a_next < s_next ? a_next : s_next;
    bool sedge = next == s_next;
    bool aedge = next == a_next;
    if (sedge)
    {
        dut->clk_sys = 1;
        if ((g_sys & 1) == 0)
        {
            dut->clk_vid = 1;
            dut->clk_rv = 1;
        }
    }
    if (aedge)
    {
        /* mf_datatable registers both the address and the output of port A
         * on CLOCK0, so datatable_q follows the address by two clk_74a
         * cycles. */
        dut->datatable_q = dt_pipe[1];
        dt_pipe[1] = dt_pipe[0];
        dt_pipe[0] = g_dt[dut->tb_pocket_dt_addr & 63];
        dut->clk_74a = 1;
    }
    dut->eval();
    if (sedge && dut->tb_pocket_tx_valid)
        g_console += (char)dut->tb_pocket_tx_data;
    if (sedge && dut->tb_pocket_rv_tx_valid)
        g_rv += (char)dut->tb_pocket_rv_tx_data;
    /* pocket_file raises each request line for a single clk_74a cycle,
     * and core_bridge_cmd queues a command on the rising edge of its
     * request line, so the lines are sampled on every tick() and not only
     * in step(). */
    {
        int r = dut->tb_pocket_ds_read, w = dut->tb_pocket_ds_write,
            o = dut->tb_pocket_ds_openfile, f = dut->tb_pocket_ds_flush,
            g = dut->tb_pocket_ds_getfile;
        if (o && !g_prev_o)
            g_req = REQ_OPEN;
        else if (f && !g_prev_f)
            g_req = REQ_FLUSH;
        else if (g && !g_prev_g)
            g_req = REQ_GETFILE;
        else if (r && !g_prev_r)
            g_req = REQ_READ;
        else if (w && !g_prev_w)
            g_req = REQ_WRITE;
        g_prev_r = r;
        g_prev_w = w;
        g_prev_o = o;
        g_prev_f = f;
        g_prev_g = g;
    }
    if (sedge)
    {
        dut->clk_sys = 0;
        dut->clk_vid = 0;
        dut->clk_rv = 0;
        s_next += 330;
        g_sys++;
    }
    if (aedge)
    {
        dut->clk_74a = 0;
        a_next += 224;
    }
    dut->eval();
}

static void a_edge()
{
    long t = a_next;
    while (a_next == t)
        tick();
}

static void host_write(uint32_t addr, uint32_t w)
{
    dut->bridge_wr = 1;
    dut->bridge_addr = addr;
    dut->bridge_wr_data = w;
    a_edge();
    dut->bridge_wr = 0;
    for (int k = 0; k < 39; k++)
        a_edge();
}

static uint32_t host_read(uint32_t addr)
{
    dut->bridge_addr = addr;
    a_edge();
    dut->bridge_rd = 1;
    a_edge();
    dut->bridge_rd = 0;
    /* The host puts the next word's address on bridge_addr before it
     * takes this word, because io_bridge_peripheral.v buffers reads by one
     * word, so a core that serves the word at the current bridge_addr
     * returns the next word. */
    dut->bridge_addr = addr + 4;
    for (int k = 0; k < 6; k++)
        a_edge();
    return dut->tb_pocket_bridge_rd_data;
}

static void host_put_bytes(uint32_t base, const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 4)
    {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; k++)
            if (i + k < n)
                w |= (uint32_t)p[i + k] << (24 - 8 * k);
        host_write(base + (uint32_t)i, w);
    }
}

static void host_get_bytes(uint32_t base, uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i += 4)
    {
        uint32_t w = host_read(base + (uint32_t)i);
        for (size_t k = 0; k < 4; k++)
            if (i + k < n)
                p[i + k] = (uint8_t)(w >> (24 - 8 * k));
    }
}

static void dt_set(uint32_t slot, uint32_t size)
{
    g_dt[slot * 2] = slot;
    g_dt[slot * 2 + 1] = size;
}

static void target_done()
{
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = 0;
    for (int k = 0; k < 4; k++)
        a_edge();
}

static void do_openfile()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint8_t param[264];
    host_get_bytes(dut->tb_pocket_param_struct, param, sizeof param);
    std::string name((const char *)param);
    /* The host does not resolve relative names, so a name without a
     * leading slash opens nothing. */
    if (name.empty() || name[0] != '/')
    {
        g_opens++;
        dut->target_dataslot_done = 1;
        dut->target_dataslot_err = 4; /* malformed path */
        for (int k = 0; k < 4; k++)
            a_edge();
        return;
    }
    std::string key = name;
    std::string parent = key.substr(0, key.rfind('/'));
    if (parent.empty())
        parent = "/";
    uint32_t flags = ((uint32_t)param[256] << 24) | ((uint32_t)param[257] << 16)
                     | ((uint32_t)param[258] << 8) | (uint32_t)param[259];
    uint32_t size = ((uint32_t)param[260] << 24) | ((uint32_t)param[261] << 16)
                    | ((uint32_t)param[262] << 8) | (uint32_t)param[263];
    g_opens++;
    bool created = false;
    auto it = g_files.find(key);
    if (it == g_files.end())
    {
        if (!(flags & 1))
        {
            dut->target_dataslot_done = 1;
            dut->target_dataslot_err = 3; /* file not found */
            for (int k = 0; k < 4; k++)
                a_edge();
            return;
        }
        /* The host creates a file only when Open File has both the create
         * bit and the resize bit set. With only the create bit set, Open
         * File returns 1 and creates nothing, and it does the same for a
         * create into a folder that does not exist. */
        if (!(flags & 2) || !g_dirs.count(parent))
        {
            dut->target_dataslot_done = 1;
            dut->target_dataslot_err = 1;
            for (int k = 0; k < 4; k++)
                a_edge();
            return;
        }
        it = g_files.emplace(key, std::vector<uint8_t>()).first;
        created = true;
    }
    if (flags & 2)
        it->second.resize(size, 0);
    g_bound[slot] = key;
    dt_set(slot, (uint32_t)it->second.size());
    /* Codes 0 and 1 from Open File are both successes, and only codes 2
     * and up are failures. */
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = created ? 1 : 0;
    for (int k = 0; k < 4; k++)
        a_edge();
}

static bool slot_bound(uint32_t slot)
{
    if (slot < 16 && !g_bound[slot].empty())
        return true;
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = 1;
    for (int k = 0; k < 4; k++)
        a_edge();
    return false;
}

static void do_slotread()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t off = dut->tb_pocket_ds_slotoffset;
    uint32_t len = dut->tb_pocket_ds_length;
    uint32_t at = dut->tb_pocket_ds_bridgeaddr;
    g_reads++;
    if (!slot_bound(slot))
        return;
    std::vector<uint8_t> &f = g_files[g_bound[slot]];
    std::vector<uint8_t> chunk(len, 0);
    for (uint32_t i = 0; i < len && off + i < f.size(); i++)
        chunk[i] = f[off + i];
    host_put_bytes(at, chunk.data(), chunk.size());
    target_done();
}

static void do_slotwrite()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t off = dut->tb_pocket_ds_slotoffset;
    uint32_t len = dut->tb_pocket_ds_length;
    uint32_t at = dut->tb_pocket_ds_bridgeaddr;
    g_writes++;
    if (!slot_bound(slot))
        return;
    std::vector<uint8_t> chunk(len, 0);
    host_get_bytes(at, chunk.data(), chunk.size());
    std::vector<uint8_t> &f = g_files[g_bound[slot]];
    if (f.size() < off + len)
        f.resize(off + len, 0);
    for (uint32_t i = 0; i < len; i++)
        f[off + i] = chunk[i];
    dt_set(slot, (uint32_t)f.size());
    target_done();
}

static void do_flush()
{
    dut->target_dataslot_done = 0;
    g_flushes++;
    /* pocket_file waits in F_ARM for target_dataslot_done to fall and
     * then in F_WAIT for it to rise, so done is held low across a few
     * clk_74a edges before target_done() raises it. */
    for (int k = 0; k < 4; k++)
        a_edge();
    target_done();
}

static void do_getfile()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t at = dut->tb_pocket_resp_struct;
    g_getfiles++;
    const std::string &name = g_bound[slot];
    std::vector<uint8_t> resp(256, 0);
    for (size_t i = 0; i < name.size() && i + 1 < resp.size(); i++)
        resp[i] = (uint8_t)name[i];
    host_put_bytes(at, resp.data(), resp.size());
    target_done();
}

static void step()
{
    tick();
    if (!g_req || g_servicing)
        return;
    int req = g_req;
    g_req = REQ_NONE;
    g_servicing = 1;
    switch (req)
    {
    case REQ_OPEN: do_openfile(); break;
    case REQ_FLUSH: do_flush(); break;
    case REQ_GETFILE: do_getfile(); break;
    case REQ_READ: do_slotread(); break;
    default: do_slotwrite(); break;
    }
    g_servicing = 0;
}

static std::vector<uint8_t> read_file(const char *path)
{
    std::vector<uint8_t> v;
    FILE *f = fopen(path, "rb");
    if (!f)
        return v;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}


/* BLOB_BRIDGE and BLOB_BYTES are core_top's savestate_addr and
 * savestate_size. */
#define BLOB_BRIDGE 0x03F00000u
#define BLOB_BYTES 324944u

/* The core has until the next read strobe to drive bridge_rd_data,
 * because io_bridge_peripheral.v buffers reads by one word. HOST_GAP is
 * about one microsecond of clk_74a, which is shorter than the 88 cycles
 * that io_bridge_peripheral.v gives as the fastest host access. */
#define HOST_GAP 74

static long g_underrun_at;

static void boot_into(const std::vector<uint8_t> &rom, bool keep_card)
{
    dut = new Vtb_pocket;
    a_next = s_next = g_sys = 0;
    g_console.clear();
    g_rv.clear();
    if (!keep_card)
    {
        g_files.clear();
        g_dirs = {"/", "/Assets", "/Assets/rp6502", "/Assets/rp6502/common",
                  "/Saves", "/Saves/rp6502", "/Saves/rp6502/common"};
    }
    g_opens = g_reads = g_writes = g_flushes = g_getfiles = 0;
    memset(g_dt, 0, sizeof g_dt);
    for (auto &b : g_bound)
        b.clear();
    g_bound[0] = "/Assets/rp6502/common/file.rp6502";
    g_files[g_bound[0]] = rom;

    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
    dut->savestate_start = 0;
    dut->savestate_load = 0;
    dut->dataslot_allcomplete = 0;
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = 0;
    for (int i = 0; i < 40; i++)
        tick();

    auto *r = dut->rootp;
    tb_load_tcm(r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm0,
                r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm1,
                r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm2,
                r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm3,
                SW_BIN);

    dut->rst_n = 1;
    dut->arst_n = 1;
    for (int i = 0; i < 40000 && !dut->tb_pocket_ready; i++)
        tick();

    std::vector<uint8_t> fonts = read_file(FONTS_BIN);
    host_put_bytes(TB_STAGE_FONT_BASE, fonts.data(), fonts.size());
    std::vector<uint8_t> oemcp = read_file(OEMCP_BIN);
    host_put_bytes(TB_STAGE_OEMCP_BASE, oemcp.data(), oemcp.size());
    host_put_bytes(TB_STAGE_ROM_BASE, rom.data(), rom.size());
    dt_set(0, (uint32_t)rom.size());
    dut->rtc_epoch = 1000000000u;
    dut->rtc_valid = 1;
    host_write(0x1000000Cu, 5);
    host_write(0x10000010u, 30);
    host_write(0x10000014u, 0);

    dut->datatable_q = g_dt[1];
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
    for (int i = 0; i < 4000; i++)
        step();
}

static void boot(const std::vector<uint8_t> &rom)
{
    boot_into(rom, false);
}

static void teardown()
{
    dut->final();
    delete dut;
    dut = nullptr;
}

static bool create_state(std::vector<uint8_t> &blob)
{
    dut->savestate_start = 1;
    dut->eval();
    bool acked = false;
    for (long i = 0; i < 200000L && !acked; i++)
    {
        step();
        acked = dut->tb_pocket_savestate_start_ack;
    }
    /* pocket_sst drives the ack straight from savestate_start, so the ack
     * can be high before pocket_sst has registered the request on a
     * clk_74a edge. core_bridge_cmd also keeps savestate_start high for
     * two cycles after the ack arrives, until ST_IDLE clears it. */
    for (int k = 0; k < 4; k++)
        a_edge();
    dut->savestate_start = 0;
    dut->eval();
    if (!acked)
        return false;
    for (long i = 0; i < 80000000L && dut->tb_pocket_savestate_start_busy; i++)
    {
        step();
    }
    if (dut->tb_pocket_savestate_start_busy || !dut->tb_pocket_savestate_start_ok)
        return false;

    blob.assign(BLOB_BYTES, 0);
    for (uint32_t i = 0; i < BLOB_BYTES; i += 4)
    {
        dut->bridge_addr = BLOB_BRIDGE + i;
        a_edge();
        dut->bridge_rd = 1;
        a_edge();
        dut->bridge_rd = 0;
        dut->bridge_addr = BLOB_BRIDGE + i + 4;
        for (int k = 0; k < HOST_GAP; k++)
            a_edge();
        uint32_t w = dut->tb_pocket_bridge_rd_data;
        blob[i] = (uint8_t)(w >> 24);
        blob[i + 1] = (uint8_t)(w >> 16);
        blob[i + 2] = (uint8_t)(w >> 8);
        blob[i + 3] = (uint8_t)w;
        if (dut->tb_pocket_savestate_start_err && !g_underrun_at)
            g_underrun_at = (long)i;
    }
    return true;
}

static bool load_state(void)
{
    dut->savestate_load = 1;
    dut->eval();
    bool acked = false;
    for (long i = 0; i < 200000L && !acked; i++)
    {
        step();
        acked = dut->tb_pocket_savestate_load_ack;
    }
    for (int k = 0; k < 4; k++)
        a_edge();
    dut->savestate_load = 0;
    dut->eval();
    if (!acked)
        return false;
    for (long i = 0; i < 40000000L && dut->tb_pocket_savestate_load_busy; i++)
        step();
    if (dut->tb_pocket_savestate_load_busy)
        fprintf(stderr, "load: still busy\n");
    if (dut->tb_pocket_savestate_load_err)
        fprintf(stderr, "load: refused at word %u (saw %08X)\n",
                (unsigned)dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__bad_idx,
                dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__bad_word);
    return !dut->tb_pocket_savestate_load_busy
           && dut->tb_pocket_savestate_load_ok
           && !dut->tb_pocket_savestate_load_err;
}

#define MEM(f) (dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__##f)

static const std::string DONE = "pocket file ok";

#define SAVE_MARGIN 15000L

#define VID(f) (dut->rootp->tb_pocket__DOT__core__DOT__video__DOT__##f)

static long vid_offset(long steps)
{
    long seen = -1;
    bool prev_start = true;
    for (long i = 0; i < steps; i++)
    {
        step();
        bool start = VID(locked) && VID(x) == 0 && VID(y) == 0;
        if (start && !prev_start)
        {
            long at = (long)MEM(vid_v) * 800 + (long)MEM(vid_h);
            if (seen >= 0 && seen != at)
                return -1;
            seen = at;
        }
        prev_start = start;
    }
    return seen;
}

static void step_to_save_point(void)
{
    while (!(int)MEM(resb))
        step();
    for (long i = 0; i < SAVE_MARGIN; i++)
        step();
}

static long vid_offset_to_save(void)
{
    long seen = -1;
    bool prev_start = true;
    bool released = false;
    for (long after = 0; !released || after < SAVE_MARGIN;)
    {
        step();
        if (released)
            ++after;
        else
            released = (int)MEM(resb);
        bool start = VID(locked) && VID(x) == 0 && VID(y) == 0;
        if (start && !prev_start)
        {
            long at = (long)MEM(vid_v) * 800 + (long)MEM(vid_h);
            if (seen >= 0 && seen != at)
                return -1;
            seen = at;
        }
        prev_start = start;
    }
    return seen;
}


/* The host writes back the whole file the OS saved. On a Pocket that file
 * was measured as a 596-byte header, the blob and a 52764-byte thumbnail. */
static std::vector<uint8_t> wrap_blob(const std::vector<uint8_t> &blob,
                                      size_t head, size_t tail)
{
    std::vector<uint8_t> v;
    v.reserve(head + blob.size() + tail);
    for (size_t i = 0; i < head; i++)
        v.push_back((uint8_t)(0x5A ^ (i * 7)));
    v.insert(v.end(), blob.begin(), blob.end());
    for (size_t i = 0; i < tail; i++)
        v.push_back((uint8_t)(0xC3 ^ (i * 11)));
    return v;
}

UTEST_MAIN();

UTEST(psleep, a_running_program_survives_the_reconfigure)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);

    long align_pre = vid_offset_to_save();
    ASSERT_EQ(0u, g_console.size());
    ASSERT_TRUE((int)MEM(resb));
    ASSERT_GE(align_pre, 0L);
    uint32_t prog_pre = (uint32_t)MEM(mode0__DOT__prog_shadow);
    ASSERT_NE(0u, prog_pre);
    for (uint32_t i = 0; i < 16; i++)
    {
        MEM(xram__DOT__mem0)[i] = (uint8_t)(0xA0 + i);
        MEM(xram__DOT__mem1)[i] = (uint8_t)(0xB0 + i);
        MEM(xram__DOT__mem2)[i] = (uint8_t)(0xC0 + i);
        MEM(xram__DOT__mem3)[i] = (uint8_t)(0xD0 + i);
        MEM(mode0__DOT__cell0)[i] = (uint8_t)(0x10 + i);
        MEM(mode0__DOT__cell1)[i] = (uint8_t)(0x20 + i);
        MEM(prog__DOT__fill_e)[i] = 0x80005000u + i;
        MEM(prog__DOT__spr_c)[i] = 0x99000000u + i;
    }

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    {
        uint32_t sum = 0;
        const uint32_t words = (uint32_t)blob.size() / 4;
        for (uint32_t i = 0; i < words - 4; i++)
        {
            uint32_t w = ((uint32_t)blob[i * 4] << 24)
                         | ((uint32_t)blob[i * 4 + 1] << 16)
                         | ((uint32_t)blob[i * 4 + 2] << 8)
                         | (uint32_t)blob[i * 4 + 3];
            sum = ((sum << 1) | (sum >> 31)) + w;
        }
        uint32_t tr = ((uint32_t)blob[(words - 4) * 4] << 24)
                      | ((uint32_t)blob[(words - 4) * 4 + 1] << 16)
                      | ((uint32_t)blob[(words - 4) * 4 + 2] << 8)
                      | (uint32_t)blob[(words - 4) * 4 + 3];
        if (sum != tr)
            fprintf(stderr,
                    "blob self-check: sum %08X trailer %08X engine sum %08X "
                    "sum_idx %u summing %d hdr %02X%02X%02X%02X\n",
                    sum, tr,
                    dut->rootp
                        ->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__sum,
                    (unsigned)dut->rootp
                        ->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__sum_idx,
                    (int)dut->rootp
                        ->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__summing,
                    blob[0], blob[1], blob[2], blob[3]);
        ASSERT_EQ(sum, tr);
    }
    if (g_underrun_at)
        fprintf(stderr, "underrun at byte %ld, word %ld\n",
                g_underrun_at, g_underrun_at / 4);
    ASSERT_FALSE((int)dut->tb_pocket_savestate_start_err);

    for (long i = 0; i < 20000000L && g_console.find(DONE) == std::string::npos;
         i++)
    {
        step();
        if ((i % 4000000L) == 3999999L)
            fprintf(stderr, "after save t=%ldM pc=%04x resb=%d eng=%d "
                            "con=%zu rv=%zu\n",
                    i / 1000000L, (unsigned)MEM(cpu__DOT__pc), (int)MEM(resb),
                    (int)MEM(engine__DOT__state), g_console.size(),
                    g_rv.size());
    }
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
    ASSERT_EQ(align_pre, vid_offset(3L * 525 * 1600));

    teardown();

    boot(rom);
    ASSERT_NE(0xA0u, (uint32_t)MEM(xram__DOT__mem0)[0]);
    step_to_save_point();
    ASSERT_TRUE((int)MEM(resb));
    ASSERT_TRUE(g_console.find(DONE) == std::string::npos);

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());

    ASSERT_TRUE(load_state());
    for (uint32_t i = 0; i < 16; i++)
    {
        ASSERT_EQ(0xA0u + i, (uint32_t)MEM(xram__DOT__mem0)[i]);
        ASSERT_EQ(0xB0u + i, (uint32_t)MEM(xram__DOT__mem1)[i]);
        ASSERT_EQ(0xC0u + i, (uint32_t)MEM(xram__DOT__mem2)[i]);
        ASSERT_EQ(0xD0u + i, (uint32_t)MEM(xram__DOT__mem3)[i]);
        ASSERT_EQ(0x10u + i, (uint32_t)MEM(mode0__DOT__cell0)[i]);
        ASSERT_EQ(0x20u + i, (uint32_t)MEM(mode0__DOT__cell1)[i]);
        ASSERT_EQ(0x80005000u + i, (uint32_t)MEM(prog__DOT__fill_e)[i]);
        ASSERT_EQ(0x99000000u + i, (uint32_t)MEM(prog__DOT__spr_c)[i]);
    }

    for (long i = 0; i < 20000000L && g_console.find(DONE) == std::string::npos;
         i++)
    {
        step();
        if ((i % 2000000L) == 1999999L)
            fprintf(stderr, "t=%ldM pc=%04x resb=%d con=%zu rv=%zu\n",
                    i / 1000000L, (unsigned)MEM(cpu__DOT__pc),
                    (int)MEM(resb), g_console.size(), g_rv.size());
    }
    if (g_console.find(DONE) == std::string::npos)
        fprintf(stderr,
                "6502 pc %04x resb=%d running=%d engine=%d console=[%s] rv=[%s]\n",
                (unsigned)MEM(cpu__DOT__pc), (int)MEM(resb),
                (int)dut->rootp->tb_pocket__DOT__mach_clk_en, (int)MEM(engine__DOT__state),
                g_console.c_str(), g_rv.c_str());
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
    ASSERT_EQ(align_pre, vid_offset(3L * 525 * 1600));
    ASSERT_EQ(prog_pre, (uint32_t)MEM(mode0__DOT__prog_shadow));
    teardown();
}

UTEST(psleep, a_file_open_across_the_sleep_is_still_open)
{
    std::vector<uint8_t> rom = read_file(STREAM_ROM);
    ASSERT_GT(rom.size(), 0u);

    std::vector<uint8_t> payload;
    for (int i = 0; i < 512; i++)
        payload.push_back((uint8_t)('a' + (i % 26)));
    std::string want((const char *)payload.data(), payload.size());

    boot(rom);
    g_files["/Saves/rp6502/common/M.DAT"] = payload;

    for (long i = 0; i < 20000000L && g_console.size() < 64; i++)
        step();
    ASSERT_GE(g_console.size(), 64u);
    ASSERT_LT(g_console.size(), want.size());
    std::string before = g_console;

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot_into(other, true);
    step_to_save_point();

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    for (long i = 0; i < 2000000L && (int)MEM(resb); i++)
        step();
    ASSERT_FALSE((int)MEM(resb));
    ASSERT_TRUE(load_state());
    g_console.clear();

    for (long i = 0; i < 40000000L
                     && g_console.find("stream ok\r\n") == std::string::npos;
         i++)
        step();
    ASSERT_TRUE(g_console.find("stream ok\r\n") != std::string::npos);
    std::string want_tail = want.substr(before.size()) + "stream ok\r\n";
    if (want_tail != g_console)
    {
        size_t at = 0;
        while (at < want_tail.size() && at < g_console.size()
               && want_tail[at] == g_console[at])
            at++;
        size_t from = at > 8 ? at - 8 : 0;
        fprintf(stderr,
                "join: before=%zu want=%zu got=%zu differ at %zu\n",
                before.size(), want_tail.size(), g_console.size(), at);
        for (size_t i = from; i < at + 8; i++)
            fprintf(stderr, "  %zu want=%02x got=%02x\n", i,
                    i < want_tail.size() ? (unsigned)(uint8_t)want_tail[i] : 0,
                    i < g_console.size() ? (unsigned)(uint8_t)g_console[i] : 0);
    }
    ASSERT_TRUE(want_tail == g_console);
    teardown();
}

UTEST(psleep, a_load_into_a_running_machine_keeps_its_bindings)
{
    std::vector<uint8_t> rom = read_file(STREAM_ROM);
    ASSERT_GT(rom.size(), 0u);
    std::vector<uint8_t> payload;
    for (int i = 0; i < 512; i++)
        payload.push_back((uint8_t)('a' + (i % 26)));

    boot(rom);
    g_files["/Saves/rp6502/common/M.DAT"] = payload;
    for (long i = 0; i < 20000000L && g_console.size() < 64; i++)
        step();
    ASSERT_GE(g_console.size(), 64u);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    int opens_before = g_opens, getfiles_before = g_getfiles;
    ASSERT_TRUE(load_state());
    g_console.clear();

    for (long i = 0; i < 40000000L
                     && g_console.find("stream ok\r\n") == std::string::npos;
         i++)
        step();
    ASSERT_TRUE(g_console.find("stream ok\r\n") != std::string::npos);
    ASSERT_GT(g_getfiles, getfiles_before);
    ASSERT_EQ(opens_before, g_opens);
    teardown();
}

UTEST(psleep, a_sleep_inside_a_file_operation_still_finishes_it)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);

    for (long i = 0; i < 20000000L && g_writes == 0; i++)
        step();
    ASSERT_GT(g_writes, 0);
    ASSERT_TRUE(g_console.find(DONE) == std::string::npos);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    boot_into(rom, true);
    step_to_save_point();
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    ASSERT_TRUE(load_state());
    g_console.clear();

    for (long i = 0; i < 40000000L && g_console.find(DONE) == std::string::npos;
         i++)
        step();
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
    teardown();
}

UTEST(psleep, the_raster_registers_come_back)
{
    std::vector<uint8_t> rom = read_file(ROMS_DIR "/mode3_1bpp.rp6502");
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    for (long i = 0; i < 8000000L
                     && !(uint32_t)MEM(prog__DOT__canvas_shadow);
         i++)
        step();
    for (long i = 0; i < 2000000L; i++)
        step();

    uint32_t canvas_pre = (uint32_t)MEM(prog__DOT__canvas_shadow);
    uint32_t prog_pre = (uint32_t)MEM(mode0__DOT__prog_shadow);
    uint32_t vsync_pre = (uint32_t)MEM(prog__DOT__vsync_shadow);
    ASSERT_EQ(1u, canvas_pre);
    /* 320x240 spans two lines of timing a row, and the register counts
     * lines of timing. */
    ASSERT_EQ(480u, vsync_pre);
    ASSERT_NE(0u, prog_pre);

    uint64_t mtime_pre = (uint64_t)MEM(soc__DOT__mtime_us);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot(other);
    step_to_save_point();
    ASSERT_EQ(0u, (uint32_t)MEM(prog__DOT__canvas_shadow));
    ASSERT_EQ(480u, (uint32_t)MEM(prog__DOT__vsync_shadow));

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    ASSERT_TRUE(load_state());
    /* wake_task restores the canvas, the vsync line and the mode 0
     * scanline range while the 6502 is held in reset, and sst_engine
     * returns to S_IDLE within two clk_sys cycles of S_LD_JAM releasing
     * that reset, so the three values checked at S_IDLE were written by
     * the firmware and not by the restored program. */
    long guard = 0;
    while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);

    uint64_t mtime_post = (uint64_t)MEM(soc__DOT__mtime_us);
    ASSERT_GE(mtime_post, mtime_pre);
    ASSERT_LT(mtime_post, mtime_pre + 20000u);

    ASSERT_EQ(canvas_pre, (uint32_t)MEM(prog__DOT__canvas_shadow));
    ASSERT_EQ(prog_pre, (uint32_t)MEM(mode0__DOT__prog_shadow));
    ASSERT_EQ(vsync_pre, (uint32_t)MEM(prog__DOT__vsync_shadow));
    teardown();
}

UTEST(psleep, a_file_with_no_blob_in_it_is_refused_and_the_session_lives)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    step_to_save_point();
    ASSERT_TRUE((int)MEM(resb));

    /* The 8192 bytes of junk hold every word that sst_engine's scan can
     * read, since SCAN_CAP stops the scan at word offset 1023. No word of
     * the junk is SST_MAGIC, so the scan ends in S_LD_BAD. */
    std::vector<uint8_t> junk = wrap_blob({}, 8192, 0);
    host_put_bytes(BLOB_BRIDGE, junk.data(), junk.size());
    ASSERT_FALSE(load_state());
    ASSERT_TRUE((int)dut->tb_pocket_savestate_load_err);

    for (long i = 0; i < 20000000L && g_console.find(DONE) == std::string::npos;
         i++)
    {
        step();
        if ((i % 4000000L) == 3999999L)
            fprintf(stderr, "refused t=%ldM pc=%04x resb=%d eng=%d con=%zu "
                            "clken=%d rv=%zu\n",
                    i / 1000000L, (unsigned)MEM(cpu__DOT__pc), (int)MEM(resb),
                    (int)MEM(engine__DOT__state), g_console.size(),
                    (int)dut->rootp->tb_pocket__DOT__mach_clk_en, g_rv.size());
    }
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
    teardown();
}
