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

#include <algorithm>
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

/* The mix at each rising psg_tick since boot, so a tick's index is its
 * count in machine time. g_stop_tick is the count when the machine's clock
 * first stops after g_stop_arm, so the first tick the restored machine
 * takes is that index. */
static std::vector<uint32_t> g_mix;
/* The 6502's clock at each of those ticks: its PC and bus address, its
 * PHI2 accumulator and VIA T1, so a load that resumes the 6502 a clock off
 * shows even where the mix does not. */
struct Cpu
{
    uint32_t pc, addr, acc, t1;
    bool operator!=(const Cpu &o) const
    {
        return pc != o.pc || addr != o.addr || acc != o.acc || t1 != o.t1;
    }
};
static std::vector<Cpu> g_cpu;
static int g_tick_prev;
static bool g_stop_arm;
static long g_stop_tick;

/* Called on every edge, a_edge's included, so no tick is missed. */
static void observe()
{
    auto *r = dut->rootp;
    int t = r->tb_pocket__DOT__core__DOT__machine__DOT__psg_tick;
    if (t && !g_tick_prev)
    {
        g_mix.push_back(
            (uint32_t)(uint16_t)r->tb_pocket__DOT__core__DOT__aud_l << 16
            | (uint16_t)r->tb_pocket__DOT__core__DOT__aud_r);
        g_cpu.push_back(
            {(uint32_t)r->tb_pocket__DOT__core__DOT__machine__DOT__cpu__DOT__pc,
             (uint32_t)r->tb_pocket__DOT__core__DOT__machine__DOT__cpu_addr,
             (uint32_t)r
                 ->tb_pocket__DOT__core__DOT__machine__DOT__phi2__DOT__acc,
             (uint32_t)r
                 ->tb_pocket__DOT__core__DOT__machine__DOT__via__DOT__t1_counter});
    }
    g_tick_prev = t;
    if (g_stop_arm && !r->tb_pocket__DOT__mach_clk_en)
    {
        g_stop_arm = false;
        g_stop_tick = (long)g_mix.size();
    }
}

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
    observe();
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

/* g_flush_hold leaves the next Flush unanswered, with g_flush_owed set,
 * until the test calls target_done(). */
static bool g_flush_hold, g_flush_owed;

static void do_flush()
{
    dut->target_dataslot_done = 0;
    g_flushes++;
    /* pocket_file waits in F_ARM for target_dataslot_done to fall and
     * then in F_WAIT for it to rise, so done is held low across a few
     * clk_74a edges before target_done() raises it. */
    for (int k = 0; k < 4; k++)
        a_edge();
    if (g_flush_hold)
    {
        g_flush_hold = false;
        g_flush_owed = true;
        return;
    }
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
#define BLOB_BYTES 333136u

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
    g_mix.clear();
    g_cpu.clear();
    g_tick_prev = 0;
    g_stop_arm = false;
    g_stop_tick = -1;
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

/* Starts a save and steps until the blob is ready to read. */
static bool save_ready(void)
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
    return !dut->tb_pocket_savestate_start_busy
           && dut->tb_pocket_savestate_start_ok;
}

static void read_blob(std::vector<uint8_t> &blob)
{
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
}

static bool create_state(std::vector<uint8_t> &blob)
{
    if (!save_ready())
        return false;
    read_blob(blob);
    return true;
}

/* Ends a save by reading only its last word, which drops the request. */
static void abort_save(void)
{
    uint32_t i = BLOB_BYTES - 4;
    dut->bridge_addr = BLOB_BRIDGE + i;
    a_edge();
    dut->bridge_rd = 1;
    a_edge();
    dut->bridge_rd = 0;
    dut->bridge_addr = BLOB_BRIDGE + i + 4;
    for (int k = 0; k < HOST_GAP; k++)
        a_edge();
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
    g_files["/Assets/rp6502/common/M.DAT"] = payload;

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
    g_files["/Assets/rp6502/common/M.DAT"] = payload;
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

/* The main loop stops the program when a savestate starts to load, so the
 * program stops while its close is still waiting for the Flush reply only if
 * the close polls Flush instead of blocking the main loop. */
UTEST(psleep, a_close_waiting_on_flush_leaves_the_main_loop_running)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    g_flush_owed = false;
    g_flush_hold = true;
    boot(rom);

    for (long i = 0; i < 20000000L && !g_flush_owed; i++)
        step();
    ASSERT_TRUE(g_flush_owed);
    ASSERT_TRUE((int)MEM(resb));

    host_write(BLOB_BRIDGE, 0);
    for (long i = 0; i < 2000000L && (int)MEM(resb); i++)
        step();
    ASSERT_FALSE((int)MEM(resb));

    target_done();
    g_flush_owed = false;
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

/* The blob holds the OPL's register file, so every register of the engine
 * comes back as it was, with nothing left to replay. */
UTEST(psleep, the_opl_registers_come_back)
{
    std::vector<uint8_t> rom = read_file(OPL_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    long guard = 0;
    while (!(MEM(opl__DOT__regs)[0xB0] == 0x31 && !MEM(opl__DOT__q_count))
           && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);
    /* The views of channel 0's modulator follow a sample later. */
    for (long i = 0; i < 20000L; i++)
        step();
    uint8_t pre[256];
    for (int i = 0; i < 256; i++)
        pre[i] = MEM(opl__DOT__regs)[i];
    ASSERT_EQ(0x31, (int)pre[0xB9]);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot(other);
    step_to_save_point();
    ASSERT_EQ(0, (int)MEM(opl__DOT__regs)[0xB0]);

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    ASSERT_TRUE(load_state());
    guard = 0;
    while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);
    /* The queue was empty at the save, and still is. */
    guard = 0;
    while (MEM(opl__DOT__q_count) && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);
    for (long i = 0; i < 20000L; i++)
        step();
    for (int i = 0; i < 256; i++)
    {
        if (MEM(opl__DOT__regs)[i] != pre[i])
            fprintf(stderr, "register %02X: %02X after the load, %02X before\n", i,
                    (int)MEM(opl__DOT__regs)[i], (int)pre[i]);
        ASSERT_EQ((int)pre[i], (int)MEM(opl__DOT__regs)[i]);
    }
    teardown();
}

/* The blob holds PCM's buffer and its player, so the stream plays on from
 * the level it had at the save, with the step and format the program
 * opened it with. The program refills it on its next frame, and in three
 * frames, about 6.2 million steps, its 1050 Hz tone swings through a gate
 * of 512 about 100 times. */
UTEST(psleep, a_pcm_stream_plays_on_after_a_wake)
{
    std::vector<uint8_t> rom = read_file(PCM_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    long guard = 0;
    while (!(MEM(pcm__DOT__open) && MEM(pcm_level)) && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);
    for (long i = 0; i < 1000000L; i++)
        step();
    const uint32_t step_pre = MEM(pcm__DOT__step_m1);
    ASSERT_EQ((uint32_t)((22050ull << 32) / 48000 - 1), step_pre);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(save_ready());
    const uint32_t level_pre = MEM(pcm_level);
    ASSERT_GT(level_pre, 0u);
    read_blob(blob);
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot(other);
    step_to_save_point();
    ASSERT_FALSE((int)MEM(pcm__DOT__open));

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    ASSERT_TRUE(load_state());
    guard = 0;
    while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);
    ASSERT_TRUE((int)MEM(pcm__DOT__open));
    ASSERT_EQ(step_pre, (uint32_t)MEM(pcm__DOT__step_m1));
    ASSERT_EQ(1, (int)MEM(pcm__DOT__b16));
    ASSERT_EQ(0, (int)MEM(pcm__DOT__st));
    ASSERT_EQ(level_pre, (uint32_t)MEM(pcm_level));

    long cross = 0;
    int side = 0;
    for (long i = 0; i < 6200000L; i++)
    {
        step();
        const int l = (int16_t)dut->rootp->tb_pocket__DOT__core__DOT__aud_l;
        if (l > 512)
            cross += side < 0, side = 1;
        else if (l < -512)
            cross += side > 0, side = -1;
    }
    fprintf(stderr, "  %ld swings after the wake\n", cross);
    ASSERT_TRUE((int)MEM(resb));
    ASSERT_GT(cross, 80L);
    teardown();
}

/* The 6502's API call as the RIA window shows it: $FFF2 holds BRA's -2
 * while the call is in flight, and $FFF4 and $FFF6 hold its result in A
 * and X until the next result. */
static bool api_busy()
{
    return MEM(ria__DOT__regs)[0x12] == 0xFE;
}

/* Steps until the machine runs with the call in flight returned and the
 * 6502 in its next call, and gives the first call's result. */
static long api_next_result()
{
    long guard = 0;
    while ((api_busy() || (int)MEM(engine__DOT__state) != 0)
           && guard++ < 40000000L)
        step();
    while (!api_busy() && guard++ < 40000000L)
        step();
    if (guard >= 40000000L)
        return -1;
    return MEM(ria__DOT__regs)[0x14] | MEM(ria__DOT__regs)[0x16] << 8;
}

/* Saves made at three points inside the PCM program's write call, which
 * blocks the 6502 for about 31,000 steps while the OS pops its arguments
 * on one pass and copies on the next. Each loads into another session,
 * whose wake finishes the call, and the 6502 takes that result: the call
 * does not run again after the wake. The buffer took what it had room for
 * when the copy began, and C's wake copies with PCM parked, so C's count
 * can fall short of B's by what plays in a call, about 11 bytes. */
UTEST(psleep, an_api_call_in_flight_returns_once)
{
    std::vector<uint8_t> rom = read_file(PCM_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    long guard = 0;
    while (!(MEM(pcm__DOT__open) && MEM(pcm_level)) && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);
    for (long i = 0; i < 1000000L; i++)
        step();
    static const long into[] = {6000, 14000, 24000};
    const int n = (int)(sizeof into / sizeof into[0]);
    std::vector<uint8_t> blob[n];
    long b_ax[n];
    for (int k = 0; k < n; k++)
    {
        for (int tries = 0; tries < 8; tries++)
        {
            guard = 0;
            while (api_busy() && guard++ < 20000000L)
                step();
            while (!api_busy() && guard++ < 20000000L)
                step();
            for (long i = 0; i < into[k]; i++)
                step();
            ASSERT_TRUE(save_ready());
            if (api_busy())
            {
                read_blob(blob[k]);
                break;
            }
            abort_save();
            guard = 0;
            while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
                step();
        }
        ASSERT_FALSE(blob[k].empty());
        b_ax[k] = api_next_result();
        ASSERT_GT(b_ax[k], 0L);
    }
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    for (int k = 0; k < n; k++)
    {
        boot(other);
        step_to_save_point();
        std::vector<uint8_t> file = wrap_blob(blob[k], 596, 52764);
        host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
        ASSERT_TRUE(load_state());
        long c_ax = api_next_result();
        teardown();
        fprintf(stderr, "  saved %ld steps into the call: B took %ld, C %ld\n",
                into[k], b_ax[k], c_ax);
        ASSERT_LE(c_ax, b_ax[k]);
        ASSERT_GE(c_ax, b_ax[k] - 16);
    }
}

/* The blob's words, high byte first, and its audio region, the last 2048
 * words before the trailer. */
#define BLOB_WORDS (BLOB_BYTES / 4)
#define B_AUD (BLOB_WORDS - 4 - 2048)

static uint32_t blob_get(const std::vector<uint8_t> &b, uint32_t i)
{
    return (uint32_t)b[i * 4] << 24 | (uint32_t)b[i * 4 + 1] << 16
           | (uint32_t)b[i * 4 + 2] << 8 | (uint32_t)b[i * 4 + 3];
}

static void blob_put(std::vector<uint8_t> &b, uint32_t i, uint32_t w)
{
    b[i * 4] = (uint8_t)(w >> 24);
    b[i * 4 + 1] = (uint8_t)(w >> 16);
    b[i * 4 + 2] = (uint8_t)(w >> 8);
    b[i * 4 + 3] = (uint8_t)w;
}

static void blob_reseal(std::vector<uint8_t> &b)
{
    uint32_t sum = 0;
    for (uint32_t i = 0; i < BLOB_WORDS - 4; i++)
        sum = ((sum << 1) | (sum >> 31)) + blob_get(b, i);
    blob_put(b, BLOB_WORDS - 4, sum);
}

/* The audio session: the PSG, the OPL, PCM and the bell, a sample in
 * 1050 clocks, so a tick is about 2,600 steps. */
#define WINDOW 4800L

static void run_to(char c, long settle)
{
    for (long i = 0; i < 80000000L && g_console.find(c) == std::string::npos;
         i++)
        step();
    for (long i = 0; i < settle; i++)
        step();
}

static void run_ticks(size_t n)
{
    for (long i = 0; i < 200000000L && g_mix.size() < n; i++)
        step();
}

/* The engines' counts that the mix does not show. */
struct Tail
{
    uint32_t drops, level;
};
static Tail tail()
{
    return {(uint32_t)MEM(opl_drops), (uint32_t)MEM(pcm_level)};
}

/* got from at against ref from rat, for n ticks: the first that differs,
 * or -1. */
static long first_diff(const std::vector<uint32_t> &ref, long rat,
                       const std::vector<uint32_t> &got, long at, long n)
{
    for (long i = 0; i < n; i++)
        if (ref[(size_t)(rat + i)] != got[(size_t)(at + i)])
        {
            fprintf(stderr, "  tick %ld: %08X, the reference %08X\n", i,
                    got[(size_t)(at + i)], ref[(size_t)(rat + i)]);
            return i;
        }
    return -1;
}

/* The same for the 6502's clock. */
static long first_cpu_diff(const std::vector<Cpu> &ref, long rat,
                           const std::vector<Cpu> &got, long at, long n)
{
    for (long i = 0; i < n; i++)
    {
        const Cpu &a = ref[(size_t)(rat + i)], &b = got[(size_t)(at + i)];
        if (a != b)
        {
            fprintf(stderr, "  tick %ld: pc %04X addr %04X acc %u t1 %u, "
                            "the reference %04X %04X %u %u\n",
                    i, b.pc, b.addr, b.acc, b.t1, a.pc, a.addr, a.acc, a.t1);
            return i;
        }
    }
    return -1;
}

/* C: another program's session takes the blob, and records n ticks from
 * the first its restored machine takes. */
struct Loaded
{
    std::vector<uint32_t> mix;
    std::vector<Cpu> cpu;
    long at;
    Tail end;
};
static bool load_into_other(const std::vector<uint8_t> &blob, long n,
                            Loaded &c)
{
    std::vector<uint8_t> other = read_file(OPL_BEL_ROM);
    boot(other);
    step_to_save_point();
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    g_stop_arm = true;
    if (!load_state())
        return false;
    c.at = g_stop_tick;
    run_ticks((size_t)(c.at + n));
    c.end = tail();
    c.mix = g_mix;
    c.cpu = g_cpu;
    teardown();
    return true;
}

/* A never saves; B saves at two request points 500 clocks apart and plays
 * on; C, another program's session, loads B's blob. B and C match A tick
 * for tick across the window, in the mix and the 6502's clock, and in the
 * OPL's drop count and PCM's level at its end. The window opens 5 ms after
 * W with PCM three quarters full, the bell in its release and the loop
 * writing through RW0. */
UTEST(psleep, the_audio_comes_back_sample_for_sample)
{
    std::vector<uint8_t> rom = read_file(AUD_SST_ROM);
    ASSERT_GT(rom.size(), 0u);
    std::vector<uint8_t> blob[2];
    std::vector<uint32_t> b_mix[2];
    std::vector<Cpu> b_cpu[2];
    long at[2];
    Tail b_end[2];
    for (int k = 0; k < 2; k++)
    {
        boot(rom);
        run_to('W', 620000L + 1235L * k);
        g_stop_arm = true;
        ASSERT_TRUE(save_ready());
        at[k] = g_stop_tick;
        fprintf(stderr, "  save %d at tick %ld: level %u, queue %u\n", k,
                at[k], (unsigned)MEM(pcm_level),
                (unsigned)MEM(opl__DOT__q_count));
        ASSERT_GT((uint32_t)MEM(pcm_level), 1024u);
        read_blob(blob[k]);
        run_ticks((size_t)(at[k] + WINDOW));
        b_end[k] = tail();
        b_mix[k] = g_mix;
        b_cpu[k] = g_cpu;
        teardown();
    }

    boot(rom);
    Tail a_end[2];
    for (int k = 0; k < 2; k++)
    {
        run_ticks((size_t)(at[k] + WINDOW));
        a_end[k] = tail();
    }
    std::vector<uint32_t> a_mix = g_mix;
    std::vector<Cpu> a_cpu = g_cpu;
    teardown();

    for (int k = 0; k < 2; k++)
    {
        long nz = 0;
        for (long i = 0; i < WINDOW; i++)
            nz += a_mix[(size_t)(at[k] + i)] != 0;
        ASSERT_GT(nz, WINDOW / 2);
        ASSERT_EQ(-1L, first_diff(a_mix, at[k], b_mix[k], at[k], WINDOW));
        ASSERT_EQ(-1L, first_cpu_diff(a_cpu, at[k], b_cpu[k], at[k], WINDOW));
        ASSERT_EQ(a_end[k].drops, b_end[k].drops);
        ASSERT_EQ(a_end[k].level, b_end[k].level);

        Loaded c;
        ASSERT_TRUE(load_into_other(blob[k], WINDOW, c));
        ASSERT_EQ(-1L, first_diff(a_mix, at[k], c.mix, c.at, WINDOW));
        ASSERT_EQ(-1L, first_cpu_diff(a_cpu, at[k], c.cpu, c.at, WINDOW));
        ASSERT_EQ(a_end[k].drops, c.end.drops);
        ASSERT_EQ(a_end[k].level, c.end.level);
    }
}

/* Saves made while the 6502 floods RW0 and RW1, until one stops with the
 * snooped write in flight and one with the RW engine's write pending, both
 * bound for the OPL, whose queue is full, so a lost write shows in the
 * drop count. Each loads into another session and matches the session
 * that never saved in the mix, the 6502's clock and the count, and each
 * probe in between is a save ended after its last word. */
UTEST(psleep, a_snoop_in_flight_comes_back)
{
    std::vector<uint8_t> rom = read_file(AUD_SNOOP_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long n = 1500;
    std::vector<uint8_t> blob[2];
    long at[2] = {-1, -1};
    boot(rom);
    run_to('W', 200000L);
    int probes = 0;
    while ((at[0] < 0 || at[1] < 0) && probes < 400)
    {
        for (long i = 0; i < 4001 + 37L * (probes % 29); i++)
            step();
        g_stop_arm = true;
        ASSERT_TRUE(save_ready());
        probes++;
        bool qs = MEM(qs_we) && (MEM(qs_addr) >> 8) == 0xF0;
        bool rw = MEM(ria__DOT__xr_wr_pend)
                  && (MEM(ria__DOT__xr_wr_addr) >> 8) == 0xF0;
        int k = qs && at[0] < 0 ? 0 : rw && at[1] < 0 ? 1 : -1;
        if (k >= 0)
        {
            at[k] = g_stop_tick;
            read_blob(blob[k]);
        }
        else
            abort_save();
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
    }
    fprintf(stderr, "  %d probes: the snoop at tick %ld, the RW write at %ld\n",
            probes, at[0], at[1]);
    ASSERT_GE(at[0], 0L);
    ASSERT_GE(at[1], 0L);
    run_ticks((size_t)(std::max(at[0], at[1]) + n));
    std::vector<uint32_t> b_mix = g_mix;
    std::vector<Cpu> b_cpu = g_cpu;
    teardown();

    boot(rom);
    int first = at[0] < at[1] ? 0 : 1;
    Tail a_end[2];
    run_ticks((size_t)(at[first] + n));
    a_end[first] = tail();
    run_ticks((size_t)(at[1 - first] + n));
    a_end[1 - first] = tail();
    std::vector<uint32_t> a_mix = g_mix;
    std::vector<Cpu> a_cpu = g_cpu;
    teardown();
    for (int k = 0; k < 2; k++)
    {
        ASSERT_EQ(-1L, first_diff(a_mix, at[k], b_mix, at[k], n));
        ASSERT_EQ(-1L, first_cpu_diff(a_cpu, at[k], b_cpu, at[k], n));
        Loaded c;
        ASSERT_TRUE(load_into_other(blob[k], n, c));
        ASSERT_EQ(-1L, first_diff(a_mix, at[k], c.mix, c.at, n));
        ASSERT_EQ(-1L, first_cpu_diff(a_cpu, at[k], c.cpu, c.at, n));
        ASSERT_EQ(a_end[k].drops, c.end.drops);
    }
}

/* Saves made until one stops a clock before a PHI2 enable, which only the
 * jam may hand the restored 6502, and which nothing may launch early: B
 * and C match A in the mix and the 6502's clock. */
UTEST(psleep, a_stop_before_a_phi2_enable_comes_back)
{
    std::vector<uint8_t> rom = read_file(AUD_SST_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long n = 1500;
    std::vector<uint8_t> blob;
    long at = -1;
    boot(rom);
    run_to('W', 620000L);
    for (int probes = 0; probes < 200 && at < 0; probes++)
    {
        for (long i = 0; i < 4001 + 37L * (probes % 29); i++)
            step();
        g_stop_arm = true;
        ASSERT_TRUE(save_ready());
        if (MEM(phi2_raw_en))
        {
            at = g_stop_tick;
            read_blob(blob);
        }
        else
            abort_save();
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
    }
    ASSERT_GE(at, 0L);
    run_ticks((size_t)(at + n));
    std::vector<uint32_t> b_mix = g_mix;
    std::vector<Cpu> b_cpu = g_cpu;
    teardown();

    boot(rom);
    run_ticks((size_t)(at + n));
    std::vector<uint32_t> a_mix = g_mix;
    std::vector<Cpu> a_cpu = g_cpu;
    teardown();
    ASSERT_EQ(-1L, first_diff(a_mix, at, b_mix, at, n));
    ASSERT_EQ(-1L, first_cpu_diff(a_cpu, at, b_cpu, at, n));
    Loaded c;
    ASSERT_TRUE(load_into_other(blob, n, c));
    ASSERT_EQ(-1L, first_diff(a_mix, at, c.mix, c.at, n));
    ASSERT_EQ(-1L, first_cpu_diff(a_cpu, at, c.cpu, c.at, n));
}

/* The regs window in the blob, and its word that holds the RW engine's
 * pending write. */
#define B_REGS (16u + 64u)
#define REGS_XR 19u
/* Voice 0's gate byte in the audio session's PSG block. */
#define PSG_GATE0 0x8006u

/* At a stop, the blob holds the snoop's write and the RW engine's pending
 * byte as the machine held them. A blob holding a write that gates a
 * released voice 0, in the snoop or pending in the RW engine, starts the
 * voice once it loads, so a lost write leaves it silent. */
UTEST(psleep, a_6502_write_at_a_stop_is_saved_and_heard)
{
    std::vector<uint8_t> rom = read_file(AUD_SST_ROM);
    ASSERT_GT(rom.size(), 0u);
    std::vector<uint8_t> blob;
    boot(rom);
    run_to('W', 620000L);
    for (int probes = 0; probes < 200 && blob.empty(); probes++)
    {
        for (long i = 0; i < 4001 + 37L * (probes % 29); i++)
            step();
        ASSERT_TRUE(save_ready());
        bool released = MEM(psg__DOT__ch_adsr)[0] == 0 && !MEM(qs_we)
                        && !MEM(ria__DOT__xr_wr_pend);
        if (released && MEM(xr_busy) && MEM(ria__DOT__xr_wr_byte))
        {
            read_blob(blob);
            ASSERT_EQ((uint32_t)MEM(ria__DOT__xr_wr_byte),
                      blob_get(blob, B_REGS + REGS_XR) >> 16 & 0xFF);
        }
        else
            abort_save();
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
    }
    ASSERT_FALSE(blob.empty());
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    for (int k = 0; k < 2; k++)
    {
        std::vector<uint8_t> held = blob;
        if (k == 0)
            blob_put(held, B_AUD + 0x7A0,
                     1u << 25 | PSG_GATE0 << 8 | 0x01u);
        else
            blob_put(held, B_REGS + REGS_XR,
                     1u << 24 | 0x01u << 16 | PSG_GATE0);
        blob_reseal(held);
        boot(other);
        step_to_save_point();
        std::vector<uint8_t> file = wrap_blob(held, 596, 52764);
        host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
        ASSERT_TRUE(load_state());
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
        for (int i = 0; i < 100; i++)
            step();
        int adsr = MEM(psg__DOT__ch_adsr)[0];
        teardown();
        fprintf(stderr, "  the gate held %s: voice 0's envelope in state %d\n",
                k ? "in the RW engine" : "in the snoop", adsr);
        ASSERT_NE(0, adsr);
    }
}

/* One audio word changed in B's blob, its sum put right, makes C differ
 * from B within the window: the OPL's LFO, a PSG voice's phase, PCM's
 * phase, and the snooped write in flight. */
UTEST(psleep, a_corrupt_audio_word_diverges)
{
    std::vector<uint8_t> rom = read_file(AUD_SST_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long n = 1500;
    boot(rom);
    run_to('W', 620000L);
    g_stop_arm = true;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(save_ready());
    long at = g_stop_tick;
    read_blob(blob);
    run_ticks((size_t)(at + n));
    std::vector<uint32_t> b_mix = g_mix;
    teardown();

    std::vector<uint8_t> rom2 = read_file(AUD_SNOOP_ROM);
    boot(rom2);
    run_to('W', 200000L);
    std::vector<uint8_t> snoop;
    long sat = -1;
    for (int probes = 0; probes < 400 && sat < 0; probes++)
    {
        for (long i = 0; i < 4001 + 37L * (probes % 29); i++)
            step();
        g_stop_arm = true;
        ASSERT_TRUE(save_ready());
        if (MEM(qs_we) && (MEM(qs_addr) >> 8) == 0xF0)
        {
            sat = g_stop_tick;
            read_blob(snoop);
        }
        else
            abort_save();
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
    }
    ASSERT_GE(sat, 0L);
    run_ticks((size_t)(sat + n));
    std::vector<uint32_t> s_mix = g_mix;
    Tail s_end = tail();
    teardown();

    static const struct
    {
        uint32_t word, flip;
        bool snooped;
    } cuts[] = {
        {0x783, 1u << 3, false},   // the OPL's LFO
        {0x720, 1u << 30, false},  // voice 0's phase
        {0x791, 1u << 31, false},  // PCM's phase
        {0x7A0, 1u << 25, true},   // the snooped write in flight
    };
    for (auto &cut : cuts)
    {
        std::vector<uint8_t> bad = cut.snooped ? snoop : blob;
        blob_put(bad, B_AUD + cut.word,
                 blob_get(bad, B_AUD + cut.word) ^ cut.flip);
        blob_reseal(bad);
        Loaded c;
        ASSERT_TRUE(load_into_other(bad, n, c));
        long d = first_diff(cut.snooped ? s_mix : b_mix,
                            cut.snooped ? sat : at, c.mix, c.at, n);
        bool tail_differs = cut.snooped && c.end.drops != s_end.drops;
        fprintf(stderr, "  word %03X ^ %08X: differs at tick %ld%s\n", cut.word,
                cut.flip, d, tail_differs ? ", and in the drop count" : "");
        ASSERT_TRUE(d >= 0 || tail_differs);
    }
}

/* Saved inside the bell's 20 ms gate: C matches A tick for tick up to the
 * release, and bel_wake moves the release by the wake's length, counted
 * from the soft CPU's resume, so it lands on the first poll after A's
 * deadline, within a main-loop pass of A's. */
UTEST(psleep, a_bell_mid_tone_comes_back)
{
    std::vector<uint8_t> rom = read_file(AUD_SST_ROM);
    ASSERT_GT(rom.size(), 0u);
    /* The bell's voice leaves release only on a strike and returns on a
     * release, so it is out of release exactly while its gate is up. */
    auto gate = [] { return MEM(psg__DOT__ch_adsr)[8] != 0 ? 1 : 0; };
    auto release_tick = [&](long limit) {
        long t = -1;
        int prev = gate();
        for (long i = 0; i < 200000000L && (long)g_mix.size() < limit; i++)
        {
            step();
            int g = gate();
            if (prev && !g && t < 0)
                t = (long)g_mix.size();
            prev = g;
        }
        return t;
    };

    boot(rom);
    run_to('\a', 0);
    for (long i = 0; i < 80000000L && !gate(); i++)
        step();
    ASSERT_TRUE(gate());
    for (long i = 0; i < 370000L; i++)
        step();
    ASSERT_TRUE(gate());
    g_stop_arm = true;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    long at = g_stop_tick;
    teardown();

    boot(rom);
    long a_rel = release_tick(at + 1600);
    std::vector<uint32_t> a_mix = g_mix;
    teardown();
    ASSERT_GT(a_rel, at);

    std::vector<uint8_t> other = read_file(OPL_BEL_ROM);
    boot(other);
    step_to_save_point();
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    g_stop_arm = true;
    ASSERT_TRUE(load_state());
    long c_at = g_stop_tick;
    ASSERT_TRUE(gate());
    long c_rel = release_tick(c_at + 1600);
    std::vector<uint32_t> c_mix = g_mix;
    teardown();
    ASSERT_GT(c_rel, c_at);

    long exact = std::min(a_rel - at, c_rel - c_at);
    fprintf(stderr, "  saved at tick %ld, released %ld ticks later in A and "
                    "%ld in C\n", at, a_rel - at, c_rel - c_at);
    ASSERT_EQ(-1L, first_diff(a_mix, at, c_mix, c_at, exact));
    /* bel_task polls once a main-loop pass, about 150 us or 8 ticks. */
    long late = (c_rel - c_at) - (a_rel - at);
    ASSERT_LE(late < 0 ? -late : late, 8L);
}

/* The 6502's RAM, a byte to the low lane of each SRAM word. */
static uint16_t ram16(uint16_t a)
{
    auto &m = dut->rootp->tb_pocket__DOT__sram_chip__DOT__mem;
    return (uint16_t)((m[a] & 0xFF) | (m[a + 1] & 0xFF) << 8);
}

/* The program's clock leaves out the wake, which holds the 6502 for a few
 * milliseconds: 10 ms into the restored machine's run, C has read the
 * clock that B read at the same tick, within the millisecond a call can
 * straddle. */
UTEST(psleep, the_program_clock_leaves_out_the_wake)
{
    std::vector<uint8_t> rom = read_file(WAKECLK_ROM);
    ASSERT_GT(rom.size(), 0u);
    const size_t n = 480;
    boot(rom);
    step_to_save_point();
    g_stop_arm = true;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    long at = g_stop_tick;
    run_ticks((size_t)at + n);
    long b_clk = ram16(0x10);
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot(other);
    step_to_save_point();
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    g_stop_arm = true;
    ASSERT_TRUE(load_state());
    long c_at = g_stop_tick;
    run_ticks((size_t)c_at + n);
    long c_clk = ram16(0x10);
    teardown();
    fprintf(stderr, "  the clock at tick %zu: %ld ms in B, %ld in C\n", n,
            b_clk, c_clk);
    ASSERT_GT(b_clk, 0L);
    ASSERT_LE(c_clk - b_clk < 0 ? b_clk - c_clk : c_clk - b_clk, 1L);
}

/* A blob from before this format, version 5, is refused, and the session
 * that was running carries on. */
UTEST(psleep, a_version_5_blob_is_refused)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    step_to_save_point();
    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();
    ASSERT_EQ(6u, blob_get(blob, 1));
    blob_put(blob, 1, 5);
    blob_reseal(blob);

    boot(rom);
    step_to_save_point();
    ASSERT_TRUE((int)MEM(resb));
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    ASSERT_FALSE(load_state());
    ASSERT_TRUE((int)dut->tb_pocket_savestate_load_err);
    for (long i = 0; i < 20000000L && g_console.find(DONE) == std::string::npos;
         i++)
        step();
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
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
