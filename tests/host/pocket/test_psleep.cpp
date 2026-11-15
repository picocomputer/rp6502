/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "tb_stage.h"
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
/* Slot Writes whose data is not in the file window, the only place the
 * firmware puts a write's data. */
static int g_wild;

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
/* The clk_sys count at which the soft CPU next halts. */
static bool g_halt_arm;
static long g_halt_sys;

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
    if (g_halt_arm
        && r->tb_pocket__DOT__core__DOT__machine__DOT__wiring_sst_dbg_halted)
    {
        g_halt_arm = false;
        g_halt_sys = g_sys;
    }
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
    /* pocket_file waits for done to fall before it waits for it to rise,
     * so the refusal holds it low across a few clk_74a edges first. */
    for (int k = 0; k < 4; k++)
        a_edge();
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
    if (at != 0x20000000u)
        g_wild++;
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
#define BLOB_BRIDGE 0x03E80000u
#define BLOB_BYTES 300368u

/* The core has until the next read strobe to drive bridge_rd_data,
 * because io_bridge_peripheral.v buffers reads by one word. HOST_GAP is
 * about one microsecond of clk_74a, which is shorter than the 88 cycles
 * that io_bridge_peripheral.v gives as the fastest host access. */
#define HOST_GAP 74

static long g_underrun_at;

/* The firmware's build stamp is the word at this offset of its image. */
#define FW_STAMP 4u

static void boot_into(const std::vector<uint8_t> &rom, bool keep_card)
{
    dut = new Vtb_pocket;
    a_next = s_next = g_sys = 0;
    g_wild = 0;
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

    tb_stage_poke_firmware(dut->rootp->tb_pocket__DOT__chip__DOT__mem,
                           SW_BIN);

    dut->rst_n = 1;
    dut->arst_n = 1;
    for (int i = 0; i < 40000 && !dut->tb_pocket_ready; i++)
        tick();

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

/* The result codes of the host's two savestate commands. */
enum { RES_IDLE = 0, RES_BUSY = 1, RES_OK = 2, RES_ERR = 3 };

static void a_step()
{
    long t = a_next;
    while (a_next == t)
        step();
}

/* A savestate command as core_bridge_cmd runs it, which test_psst.cpp
 * holds the real one to. A query's result is the status lines on one
 * clk_74a edge. A request raises its line and holds it until the edge
 * that sees the ack and two more, and its result is the status lines on
 * the ack's edge. Returns -1 when no ack comes. */
static int sst_cmd(bool load, bool request, bool service)
{
    auto status = [&]() {
        int busy = load ? dut->tb_pocket_savestate_load_busy
                        : dut->tb_pocket_savestate_start_busy;
        int ok = load ? dut->tb_pocket_savestate_load_ok
                      : dut->tb_pocket_savestate_start_ok;
        int err = load ? dut->tb_pocket_savestate_load_err
                       : dut->tb_pocket_savestate_start_err;
        return err ? RES_ERR : ok ? RES_OK : busy ? RES_BUSY : RES_IDLE;
    };
    auto edge = [&]() {
        if (service)
            a_step();
        else
            a_edge();
    };
    int code = status();
    if (!request)
    {
        edge();
        return code;
    }
    if (load)
        dut->savestate_load = 1;
    else
        dut->savestate_start = 1;
    dut->eval();
    bool acked = false;
    for (long i = 0; i < 200000L && !acked; i++)
    {
        code = status();
        acked = load ? dut->tb_pocket_savestate_load_ack
                     : dut->tb_pocket_savestate_start_ack;
        edge();
    }
    edge();
    edge();
    if (load)
        dut->savestate_load = 0;
    else
        dut->savestate_start = 0;
    dut->eval();
    return acked ? code : -1;
}

/* Starts a save and steps until the blob is ready to read. */
static bool save_ready(void)
{
    if (sst_cmd(false, true, true) != RES_BUSY)
        return false;
    for (long i = 0; i < 80000000L && dut->tb_pocket_savestate_start_busy; i++)
    {
        step();
    }
    return sst_cmd(false, false, true) == RES_OK;
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

/* The host writes a blob as it writes any file, a word at a time, and
 * answers the core's file requests between the words. */
static void put_blob(const std::vector<uint8_t> &file)
{
    for (size_t i = 0; i < file.size(); i += 4)
    {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; k++)
            if (i + k < file.size())
                w |= (uint32_t)file[i + k] << (24 - 8 * k);
        dut->bridge_wr = 1;
        dut->bridge_addr = BLOB_BRIDGE + (uint32_t)i;
        dut->bridge_wr_data = w;
        a_edge();
        dut->bridge_wr = 0;
        for (int k = 0; k < 39; k++)
            a_step();
    }
}

/* A load into the running machine: busy until the engine has finished,
 * then ok or err. */
static bool load_state(void)
{
    if (sst_cmd(true, true, true) != RES_BUSY)
        return false;
    for (long i = 0; i < 40000000L && dut->tb_pocket_savestate_load_busy; i++)
        step();
    int res = sst_cmd(true, false, true);
    if (res == RES_BUSY)
        fprintf(stderr, "load: still busy\n");
    if (res == RES_ERR)
        fprintf(stderr, "load: refused at word %u (saw %08X)\n",
                (unsigned)dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__bad_idx,
                dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__bad_word);
    return res == RES_OK;
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
    put_blob(file);

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
    put_blob(file);
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
    put_blob(file);
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
    put_blob(file);
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
    put_blob(file);
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
    while (!(MEM(opl__DOT__regs)[0xB0] == 0x31 && !MEM(opl__DOT__q_len))
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
    put_blob(file);
    ASSERT_TRUE(load_state());
    guard = 0;
    while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);
    /* The queue was empty at the save, and still is. */
    guard = 0;
    while (MEM(opl__DOT__q_len) && guard++ < 20000000L)
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
    put_blob(file);
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
        put_blob(file);
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
    put_blob(file);
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
                (unsigned)MEM(opl__DOT__q_len));
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
        put_blob(file);
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
    put_blob(file);
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
    put_blob(file);
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

/* A blob from before this format, version 8, is refused, and the session
 * that was running carries on. */
/* ------------------------------------------------------------------ */
/* Sleep and wake round trips with what the tests above hold fixed made
 * random: when the save comes, how the host reads the blob, and on the wake
 * the order and pace of the slot writes, Reset Exit, the blob and the load
 * command. PWAKE_SEED is the first seed and PWAKE_N the number of trips;
 * every other PWAKE_ variable pins one choice, to minimise a failure. */

#include <cstdlib>

struct Rng
{
    uint64_t s;
    uint32_t next()
    {
        s ^= s << 13;
        s ^= s >> 7;
        s ^= s << 17;
        return (uint32_t)(s >> 20);
    }
    uint32_t below(uint32_t n) { return n ? next() % n : 0; }
};

static long env_long(const char *name, long dflt)
{
    const char *v = getenv(name);
    return v && *v ? strtol(v, nullptr, 0) : dflt;
}


#define RVPC() ((uint32_t)dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__cpu__DOT__core__DOT__decode_u__DOT__pc)
#define RVHALT() ((int)dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__wiring_sst_dbg_halted)

#define PF(f) (dut->rootp->tb_pocket__DOT__core__DOT__file__DOT__##f)

/* 0 steady, 1 jittered, 2 bursts with long pauses, 3 slow. */
static int pw_gap(Rng &r, int mode, int base)
{
    switch (mode)
    {
    case 1: return base + (int)r.below(300);
    case 2: return r.below(3000) == 0 ? base + 20000 + (int)r.below(300000)
                                      : base;
    case 3: return base * 8 + (int)r.below(1000);
    default: return base;
    }
}

static uint32_t pw_read_word(uint32_t idx, uint32_t next_idx, int gapn)
{
    dut->bridge_addr = BLOB_BRIDGE + idx * 4;
    a_edge();
    dut->bridge_rd = 1;
    a_edge();
    dut->bridge_rd = 0;
    dut->bridge_addr = BLOB_BRIDGE + next_idx * 4;
    for (int k = 0; k < gapn; k++)
        a_edge();
    return dut->tb_pocket_bridge_rd_data;
}

/* order 0 in order, 1 in order with words read again, 2 chunks out of
 * order. The last word is read last, because serving it ends the save. */
static bool pw_read_blob(Rng &r, std::vector<uint8_t> &blob, int mode,
                         int base, int order)
{
    const uint32_t words = BLOB_BYTES / 4;
    std::vector<uint32_t> seq;
    if (order == 2)
    {
        std::vector<std::pair<uint32_t, uint32_t>> chunks;
        for (uint32_t at = 0; at < words - 1;)
        {
            uint32_t n = 16 + r.below(6000);
            if (at + n > words - 1)
                n = words - 1 - at;
            chunks.push_back({at, n});
            at += n;
        }
        for (size_t i = chunks.size(); i > 1; i--)
            std::swap(chunks[i - 1], chunks[r.below((uint32_t)i)]);
        for (auto &c : chunks)
            for (uint32_t k = 0; k < c.second; k++)
                seq.push_back(c.first + k);
    }
    else
    {
        for (uint32_t i = 0; i < words - 1; i++)
        {
            seq.push_back(i);
            if (order == 1 && r.below(200) == 0)
            {
                uint32_t back = r.below(i + 1);
                uint32_t n = 1 + r.below(40);
                for (uint32_t k = 0; k < n && back + k < words - 1; k++)
                    seq.push_back(back + k);
            }
        }
    }
    seq.push_back(words - 1);
    blob.assign(BLOB_BYTES, 0);
    bool under = false;
    for (size_t k = 0; k < seq.size(); k++)
    {
        uint32_t i = seq[k];
        uint32_t nx = k + 1 < seq.size() ? seq[k + 1] : i + 1;
        uint32_t w = pw_read_word(i, nx, pw_gap(r, mode, base));
        blob[i * 4] = (uint8_t)(w >> 24);
        blob[i * 4 + 1] = (uint8_t)(w >> 16);
        blob[i * 4 + 2] = (uint8_t)(w >> 8);
        blob[i * 4 + 3] = (uint8_t)w;
        if (dut->tb_pocket_savestate_start_err)
            under = true;
    }
    return !under;
}

static bool pw_blob_sum_ok(const std::vector<uint8_t> &b)
{
    uint32_t sum = 0;
    const uint32_t words = BLOB_BYTES / 4;
    for (uint32_t i = 0; i < words - 4; i++)
        sum = ((sum << 1) | (sum >> 31)) + blob_get(b, i);
    return sum == blob_get(b, words - 4);
}

/* One host write, then gapn clk_74a cycles in which the host either
 * answers the core's file requests or leaves them waiting. */
static void pw_write(uint32_t addr, uint32_t w, int gapn, bool service)
{
    dut->bridge_wr = 1;
    dut->bridge_addr = addr;
    dut->bridge_wr_data = w;
    a_edge();
    dut->bridge_wr = 0;
    if (!service)
    {
        for (int k = 0; k < gapn; k++)
            a_edge();
        return;
    }
    long until = a_next + (long)gapn * 224;
    while (a_next < until)
        step();
}

static void pw_put(Rng &r, uint32_t base, const std::vector<uint8_t> &v,
                   int mode, int gapbase, bool service, bool shuffle)
{
    std::vector<std::pair<size_t, size_t>> chunks;
    if (shuffle)
    {
        for (size_t at = 0; at < v.size();)
        {
            size_t n = 4 * (64 + r.below(8000));
            if (at + n > v.size())
                n = v.size() - at;
            chunks.push_back({at, n});
            at += n;
        }
        for (size_t i = chunks.size(); i > 1; i--)
            std::swap(chunks[i - 1], chunks[r.below((uint32_t)i)]);
    }
    else
        chunks.push_back({0, v.size()});
    for (auto &c : chunks)
        for (size_t i = c.first; i < c.first + c.second; i += 4)
        {
            uint32_t w = 0;
            for (size_t k = 0; k < 4; k++)
                if (i + k < v.size())
                    w |= (uint32_t)v[i + k] << (24 - 8 * k);
            pw_write(base + (uint32_t)i, w, pw_gap(r, mode, gapbase),
                     service);
        }
}

struct Trip
{
    long seed;
    int prog;         /* 0 file, 1 aud_snoop, 2 mode3 */
    long save_at;     /* steps after the program's mark */
    int rmode, rbase, rorder;
    int refuse;       /* 0 good blob, 1 other build, 2 corrupt word,
                       * 3 other version, 4 no blob in the file */
    int fw_bridge;    /* firmware through the bridge, not poked */
    int fw_first;     /* firmware slot before the ROM slot */
    long drb;         /* steps from Reset Exit to the first blob word,
                       * -1 the blob before Reset Exit */
    int wmode, wbase, wshuffle;
    int service;      /* the host answers file requests: 1 always, 2 not
                       * while it writes the blob, 0 not from the blob's
                       * first word until the load is answered */
    long dbl;         /* steps from the last blob word to the load */
    int head_pad;     /* bytes added to the 596-byte header */
    int seq;          /* SEQ_: where Reset Exit comes */
    int repeat;       /* the load asked for again: 1 at once, 2 while the
                       * first is in flight, 3 after the wake is over */
    long rep_gap;     /* steps to the repeat, for 2 */
};

/* A wake's Reset Exit against its blob and its load command. Analogue
 * documents no order, and cores report the load arriving in reset. */
enum
{
    /* Reset Exit, then the load. drb puts the blob before Reset Exit or
     * so long after it, and dbl puts the load after the blob. */
    SEQ_EXIT_FIRST = 0,
    /* The blob and the load in reset, and Reset Exit once the load has
     * answered ok. */
    SEQ_LOAD_WAIT_EXIT,
    /* The blob and the load in reset, and Reset Exit with no look at the
     * answer. */
    SEQ_LOAD_EXIT,
    /* The load in reset, then the blob, then Reset Exit. */
    SEQ_LOAD_BLOB_EXIT,
    /* The blob as SEQ_EXIT_FIRST places it, and no load at all. */
    SEQ_NO_LOAD
};

static void pw_pick(Trip &t, Rng &r)
{
    t.prog = (int)env_long("PWAKE_PROG", (long)r.below(3));
    t.rmode = (int)env_long("PWAKE_RMODE", (long)r.below(4));
    t.rbase = (int)env_long("PWAKE_RBASE", r.below(3) ? 86 : 74);
    t.rorder = (int)env_long("PWAKE_RORDER", (long)r.below(3));
    t.refuse = (int)env_long("PWAKE_REFUSE", 0);
    t.fw_bridge = (int)env_long("PWAKE_FWBRIDGE", (long)r.below(2));
    t.fw_first = (int)env_long("PWAKE_FWFIRST", (long)r.below(2));
    long drb;
    switch (r.below(6))
    {
    case 0: drb = -1; break;
    case 1: drb = (long)r.below(3000); break;
    case 2: drb = (long)r.below(3300000); break;
    case 3: drb = 3000000 + (long)r.below(600000); break;
    case 4: drb = (long)r.below(400000); break;
    default: drb = 3200000 + (long)r.below(4000000); break;
    }
    t.drb = env_long("PWAKE_DRB", drb);
    t.wmode = (int)env_long("PWAKE_WMODE", (long)r.below(3));
    t.wbase = (int)env_long("PWAKE_WBASE", 39 + (long)r.below(80));
    t.wshuffle = (int)env_long("PWAKE_WSHUFFLE", r.below(5) == 0);
    t.service = (int)env_long("PWAKE_SERVICE", (long)r.below(3));
    long dbl;
    switch (r.below(4))
    {
    case 0: dbl = 0; break;
    case 1: dbl = (long)r.below(3000); break;
    case 2: dbl = (long)r.below(600000); break;
    default: dbl = 600000 + (long)r.below(4000000); break;
    }
    t.dbl = env_long("PWAKE_DBL", dbl);
    t.head_pad = (int)env_long("PWAKE_HEADPAD", r.below(4) ? 0
                                                 : 4 * (long)r.below(100));
    long span = t.prog == 0 ? 400000 : 2500000;
    t.save_at = env_long("PWAKE_SAVEAT", (long)r.below((uint32_t)span));
    /* A trip with no load is not a round trip, so it is only pinned. */
    uint32_t seq = r.below(7);
    t.seq = (int)env_long("PWAKE_SEQ", seq < 4 ? SEQ_EXIT_FIRST
                                               : (long)seq - 3);
    uint32_t rep = r.below(8);
    t.repeat = (int)env_long("PWAKE_REPEAT", rep < 3 ? (long)rep + 1 : 0);
    t.rep_gap = env_long("PWAKE_REPGAP", (long)r.below(3000000));
}

static void pw_print(const Trip &t, const char *what)
{
    fprintf(stderr,
            "PWAKE %s seed=%ld prog=%d save_at=%ld rmode=%d rbase=%d "
            "rorder=%d refuse=%d fwbridge=%d fwfirst=%d drb=%ld wmode=%d "
            "wbase=%d wshuffle=%d service=%d dbl=%ld headpad=%d seq=%d "
            "repeat=%d repgap=%ld\n",
            what, t.seed, t.prog, t.save_at, t.rmode, t.rbase, t.rorder,
            t.refuse, t.fw_bridge, t.fw_first, t.drb, t.wmode, t.wbase,
            t.wshuffle, t.service, t.dbl, t.head_pad, t.seq, t.repeat,
            t.rep_gap);
}

static std::vector<uint8_t> pw_rom(int prog)
{
    /* 3 is any ROM, a game say, named by PWAKE_ROM. */
    if (prog == 3)
        return read_file(getenv("PWAKE_ROM") ? getenv("PWAKE_ROM") : "");
    return read_file(prog == 0 ? FILE_ROM
                     : prog == 1 ? AUD_SNOOP_ROM
                                 : ROMS_DIR "/mode3_1bpp.rp6502");
}

/* Steps to the program's mark: the 6502 out of reset, the audio program's
 * W, or the picture program's canvas. */
static bool pw_to_mark(int prog)
{
    long guard = 0;
    if (prog == 0)
        while (!(int)MEM(resb) && guard++ < 20000000L)
            step();
    else if (prog == 1)
        while (g_console.find('W') == std::string::npos
               && guard++ < 80000000L)
            step();
    else
    {
        long lim = prog == 3 ? 80000000L : 20000000L;
        while (!(uint32_t)MEM(prog__DOT__canvas_shadow) && guard++ < lim)
            step();
        if (prog == 3)
            fprintf(stderr, "  the ROM set its canvas %ld sys clocks after "
                            "boot, resb %d\n", g_sys, (int)MEM(resb));
        return guard < lim;
    }
    return guard < 20000000L || (prog == 1 && guard < 80000000L);
}

#define SST(f) (dut->rootp->tb_pocket__DOT__core__DOT__sst__DOT__##f)

static void pw_run(long n, bool service)
{
    for (long i = 0; i < n; i++)
        if (service)
            step();
        else
            tick();
}

static void pw_exit_reset()
{
    dut->datatable_q = g_dt[1];
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
}

/* Steps until the load is over: the engine idle and the firmware done
 * with it. A load asked for in reset answers ok from its request on, and
 * one asked of the running machine answers busy and then ok or err. The
 * host answers no file request before the load's answer when quiet is
 * set. Returns false when the load never ends or a status line is
 * wrong. */
static bool pw_load_over(bool early, bool quiet)
{
    bool fine = true;
    long i = 0;
    for (; i < 60000000L
           && (SST(load_act) || (int)MEM(engine__DOT__state) != 0);
         i++)
    {
        bool busy = dut->tb_pocket_savestate_load_busy;
        if (early && (busy || !dut->tb_pocket_savestate_load_ok))
            fine = false;
        if (quiet && busy)
            tick();
        else
            step();
    }
    if (i >= 60000000L)
        return false;
    int res = sst_cmd(true, false, true);
    bool refused = (int)MEM(engine__DOT__ld_bad);
    if (res != (early || !refused ? RES_OK : RES_ERR)
        || early != (bool)SST(load_early))
        fine = false;
    if (!fine)
        fprintf(stderr, "  wake: the status lines are wrong, the answer %d "
                        "with the load %s\n", res,
                refused ? "refused" : "taken");
    return fine;
}

/* Steps into the 6502's API call of that number, to the firmware's k'th
 * write to a file register during the call. Of a write these are the slot
 * and the resize that grows the file, then the slot, the offset, the
 * address, the length and the write itself. */
static bool pw_to_file_reg(long op, long k)
{
    bool prev = false;
    for (long i = 0; i < 40000000L && k > 0; i++)
    {
        step();
        bool wr = PF(reg_we) && api_busy()
                  && MEM(ria__DOT__regs)[0x0F] == (uint8_t)op;
        if (wr && !prev)
            k--;
        prev = wr;
    }
    return k == 0;
}

/* A wake as the Pocket makes it: a fresh core in reset, the data slots
 * written, and then Reset Exit, the blob and the load command in the
 * trip's order. Returns 0 for a blob loaded, 1 for one refused, 2 for a
 * load that never ended or a wrong answer, and 3 where no load was asked
 * for. */
static int pw_wake(Rng &r, const Trip &t, const std::vector<uint8_t> &rom,
                   const std::vector<uint8_t> &file)
{
    dut = new Vtb_pocket;
    a_next = s_next = g_sys = 0;
    g_wild = 0;
    g_console.clear();
    g_rv.clear();
    g_mix.clear();
    g_cpu.clear();
    g_tick_prev = 0;
    g_stop_arm = false;
    g_stop_tick = -1;
    g_req = REQ_NONE;
    g_servicing = 0;
    g_prev_r = g_prev_w = g_prev_o = g_prev_f = g_prev_g = 0;
    memset(g_dt, 0, sizeof g_dt);
    dt_pipe[0] = dt_pipe[1] = 0;
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

    std::vector<uint8_t> fw = tb_stage_firmware(SW_BIN);
    if (t.refuse == 1)
        fw[FW_STAMP] ^= 1;
    if (!t.fw_bridge)
        tb_stage_poke(dut->rootp->tb_pocket__DOT__chip__DOT__mem,
                      TB_STAGE_MEM_BASE, fw);

    dut->rst_n = 1;
    dut->arst_n = 1;
    for (int i = 0; i < 40000 && !dut->tb_pocket_ready; i++)
        tick();

    for (int pass = 0; pass < 2; pass++)
    {
        bool fw_now = (pass == 0) == (t.fw_first != 0);
        if (fw_now && t.fw_bridge)
            pw_put(r, TB_STAGE_MEM_BASE, fw, t.wmode, t.wbase, false, false);
        if (!fw_now)
            pw_put(r, TB_STAGE_ROM_BASE, rom, t.wmode, t.wbase, false, false);
    }
    dt_set(0, (uint32_t)rom.size());
    dut->rtc_epoch = 1000000000u;
    dut->rtc_valid = 1;
    host_write(0x1000000Cu, 5);
    host_write(0x10000010u, 30);
    host_write(0x10000014u, 0);

    auto put = [&](bool service) {
        pw_put(r, BLOB_BRIDGE, file, t.wmode, t.wbase, service,
               t.wshuffle != 0);
    };
    bool early = t.seq == SEQ_LOAD_WAIT_EXIT || t.seq == SEQ_LOAD_EXIT
                 || t.seq == SEQ_LOAD_BLOB_EXIT;
    bool quiet = t.service == 0;
    int want = early ? RES_OK : RES_BUSY;
    bool fine = true;
    auto ask = [&]() {
        int code = sst_cmd(true, true, !quiet);
        if (code != want)
        {
            fprintf(stderr, "  wake: the load request answered %d\n", code);
            fine = false;
        }
    };

    if (early)
    {
        if (t.seq != SEQ_LOAD_BLOB_EXIT)
            put(false);
        ask();
        if (t.repeat == 1)
            ask();
        if (t.seq == SEQ_LOAD_WAIT_EXIT
            && sst_cmd(true, false, false) != RES_OK)
            fine = false;
        if (t.seq == SEQ_LOAD_BLOB_EXIT)
            put(false);
        g_stop_arm = true;
        pw_exit_reset();
    }
    else
    {
        if (t.drb < 0)
            put(false);
        pw_exit_reset();
        /* PWAKE_BLOBOP starts the blob with the fresh firmware inside a
         * file call, as PWAKE_SAVEOP places a save. */
        long blob_op = env_long("PWAKE_BLOBOP", 0);
        if (blob_op && !pw_to_file_reg(blob_op, env_long("PWAKE_BLOBOP_K", 1)))
            return 2;
        if (t.drb >= 0)
        {
            if (!blob_op)
                pw_run(t.drb, true);
            put(t.service == 1);
        }
        if (t.seq == SEQ_NO_LOAD)
        {
            g_console.clear();
            return 3;
        }
        /* PWAKE_LOADARM asks for the load once the fresh firmware has
         * written a file command's slot and not yet the command. */
        if (env_long("PWAKE_LOADARM", 0))
        {
            long i = 0;
            for (; i < 80000000L && !PF(armed); i++)
                step();
            if (i >= 80000000L)
                return 2;
        }
        else
            pw_run(t.dbl, !quiet);
        g_stop_arm = true;
        g_console.clear();
        fprintf(stderr, "  wake: at the load pocket_file busy=%d fstate=%d "
                        "op=%d, request waiting=%d, the fresh soft CPU at "
                        "%x\n",
                (int)PF(busy), (int)PF(fstate), (int)PF(r_op), g_req, RVPC());
        ask();
        if (t.repeat == 1)
            ask();
    }
    if (t.repeat == 2)
    {
        /* While the engine loads, or once it is done and the firmware is
         * putting back what the blob does not hold. */
        for (long i = 0; i < t.rep_gap && !(int)MEM(engine__DOT__done_q)
                         && SST(load_act);
             i++)
            if (quiet && dut->tb_pocket_savestate_load_busy)
                tick();
            else
                step();
        if (SST(load_act))
        {
            want = dut->tb_pocket_savestate_load_busy ? RES_BUSY : RES_OK;
            if (dut->tb_pocket_savestate_load_err)
                want = RES_ERR;
            fprintf(stderr, "  wake: asked again with the engine in state %d "
                            "and the first answer %d\n",
                    (int)MEM(engine__DOT__state), want);
            ask();
        }
    }
    if (!pw_load_over(early, quiet))
        return 2;
    bool refused = (int)MEM(engine__DOT__ld_bad);
    if (t.repeat == 3 && !refused)
    {
        /* The wake is over and the program runs. This is a second load,
         * and it puts the program back at the save once more. */
        pw_run(200000 + t.rep_gap, true);
        g_stop_arm = true;
        g_console.clear();
        want = RES_BUSY;
        ask();
        if (!pw_load_over(false, quiet))
            return 2;
        refused = (int)MEM(engine__DOT__ld_bad);
    }
    if (refused)
        fprintf(stderr, "load: refused at word %u (saw %08X)\n",
                (unsigned)MEM(engine__DOT__bad_idx),
                (uint32_t)MEM(engine__DOT__bad_word));
    if (!fine)
        return 2;
    return refused ? 1 : 0;
}

/* The console's text: each cell's glyph, the first byte of its two words
 * in mode 0's cell memory. */
static std::string pw_screen()
{
    std::string s;
    for (int i = 0; i < 15360; i += 2)
    {
        uint8_t c = MEM(mode0__DOT__cell0)[i];
        s += c >= 32 && c < 127 ? (char)c : ' ';
    }
    return s;
}

/* The line the console shows for a wake that does not resume. */
static std::string pw_line(const Trip &t)
{
    if (t.seq == SEQ_NO_LOAD)
        return "wake: no load came ";
    static const char *const why[] = {"", "build differs", "checksum differs",
                                      "version differs", "no header"};
    bool early = t.seq != SEQ_EXIT_FIRST;
    return std::string("wake: ") + why[t.refuse]
           + (early ? " (asked in reset) " : " ");
}

/* The 6502 is out of reset and in the program, and the screen is the
 * program's and not the console's. The picture program draws and stops,
 * at rest_pc, and the others keep moving. */
static bool pw_program_runs(uint32_t canvas, uint32_t rest_pc)
{
    if (!(int)MEM(resb) || (uint32_t)MEM(prog__DOT__canvas_shadow) != canvas)
        return false;
    std::set<uint32_t> pcs;
    for (int i = 0; i < 20000; i++)
    {
        step();
        pcs.insert((uint32_t)MEM(cpu__DOT__pc));
    }
    return pcs.size() > 1 || *pcs.begin() == rest_pc;
}

static void pw_state(const char *tag)
{
    /* Where the soft CPU is: the addresses it visits in 20,000 steps. */
    {
        std::set<uint32_t> pcs;
        for (int i = 0; i < 20000; i++)
        {
            step();
            pcs.insert(RVPC());
        }
        fprintf(stderr, "  %s: the soft CPU visits %zu addresses, %x to %x:",
                tag, pcs.size(), *pcs.begin(), *pcs.rbegin());
        int k = 0;
        for (uint32_t pc : pcs)
            if (k++ < 24)
                fprintf(stderr, " %x", pc);
        fprintf(stderr, "\n  %s: pocket_file busy=%d fstate=%d op=%d "
                        "tmo_flag=%d, write queue empty=%d, host done=%d, "
                        "request waiting=%d\n",
                tag, (int)PF(busy), (int)PF(fstate), (int)PF(r_op),
                (int)PF(tmo_flag),
                (int)dut->rootp->tb_pocket__DOT__core__DOT__bridge__DOT__wf_empty,
                (int)dut->target_dataslot_done, g_req);
    }
    std::string rv = g_rv.size() > 400 ? g_rv.substr(g_rv.size() - 400)
                                       : g_rv;
    fprintf(stderr,
            "  %s: resb=%d pc=%04x engine=%d clken=%d canvas=%u slot=%u "
            "console=[%s] rv=[%s]\n",
            tag, (int)MEM(resb), (unsigned)MEM(cpu__DOT__pc),
            (int)MEM(engine__DOT__state),
            (int)dut->rootp->tb_pocket__DOT__mach_clk_en,
            (unsigned)MEM(prog__DOT__canvas_shadow),
            (unsigned)MEM(soc__DOT__mmio_slot_len), g_console.c_str(),
            rv.c_str());
}

/* One trip. Returns 0 when the wake did what it should. */
static int pw_trip(Trip &t)
{
    Rng r{(uint64_t)t.seed * 0x9E3779B97F4A7C15ull + 1};
    for (int i = 0; i < 8; i++)
        r.next();
    pw_pick(t, r);
    pw_print(t, "trip");
    std::vector<uint8_t> rom = pw_rom(t.prog);
    if (rom.empty())
        return 100;
    const long n = 1500;

    /* B: the session that sleeps. It plays on after the save, and what it
     * does then is what the wake has to do. */
    boot(rom);
    if (!pw_to_mark(t.prog))
    {
        pw_print(t, "FAIL no-mark");
        teardown();
        return 101;
    }
    uint32_t mark_canvas = (uint32_t)MEM(prog__DOT__canvas_shadow);
    for (long i = 0; i < t.save_at; i++)
        step();
    /* PWAKE_SAVEPC asks for the save as the soft CPU reaches an address,
     * PWAKE_SAVEPC_D that many steps after it. */
    if (long at_pc = env_long("PWAKE_SAVEPC", 0))
    {
        uint32_t prev = 0;
        for (long i = 0; i < 4000000L; i++)
        {
            uint32_t pc = RVPC();
            if (pc == (uint32_t)at_pc && prev != pc)
                break;
            prev = pc;
            step();
        }
        for (long i = env_long("PWAKE_SAVEPC_D", 0); i > 0; i--)
            step();
    }
    /* PWAKE_SAVEWIN asks for the save as the firmware's wake task reads
     * SST_CTL, the first read after a pause, so the soft CPU halts between
     * that read and the main loop's checks that follow it. */
    if (env_long("PWAKE_SAVEWIN", 0))
    {
        long last = g_sys;
        for (long i = 0; i < 4000000L; i++)
        {
            step();
            if (dut->rootp->tb_pocket__DOT__core__DOT____Vcellinp__sst__stb)
            {
                if (g_sys - last > 500)
                    break;
                last = g_sys;
            }
        }
    }
    if (long d = env_long("PWAKE_SAVEWIN_D", 0))
        for (long was = g_sys; g_sys - was < d;)
            step();
    /* PWAKE_SAVEOP asks for the save inside the 6502's API call of that
     * number, at the firmware's PWAKE_SAVEOP_K'th write to a file register
     * during the call. */
    if (long op = env_long("PWAKE_SAVEOP", 0))
    {
        if (!pw_to_file_reg(op, env_long("PWAKE_SAVEOP_K", 1)))
        {
            pw_print(t, "FAIL no-such-write");
            teardown();
            return 104;
        }
    }
    if (t.prog == 0 && g_console.find(DONE) != std::string::npos)
    {
        pw_print(t, "skip saved-after-done");
        teardown();
        return 0;
    }
    g_stop_arm = true;
    g_halt_arm = true;
    long asked = g_sys;
    if (!save_ready())
    {
        pw_print(t, "FAIL save-not-ready");
        pw_state("save");
        teardown();
        return 102;
    }
    long b_at = g_stop_tick;
    /* The card as the sleep leaves it. */
    std::map<std::string, std::vector<uint8_t>> card = g_files;
    fprintf(stderr, "  saved with the soft CPU halted at %x, %ld clocks "
                    "after the save was asked for\n", RVPC(),
            g_halt_sys - asked);
    std::vector<uint8_t> blob;
    /* PWAKE_POKE moves what the Pocket can move while the machine is
     * stopped, between the engine's sum and the host's read: the clock,
     * the buttons and the sticks. */
    if (long poke = env_long("PWAKE_POKE", 0))
    {
        if (poke & 1)
            dut->rtc_epoch = dut->rtc_epoch + 1;
        if (poke & 2)
        {
            dut->cont1_key = 0x0000FFFFu;
            dut->cont1_joy = 0x12345678u;
            dut->cont1_trig = 0xABCD;
        }
        for (int k = 0; k < 2000; k++)
            a_edge();
    }
    bool rd_ok = pw_read_blob(r, blob, t.rmode, t.rbase, t.rorder);
    bool sum_ok = pw_blob_sum_ok(blob);
    if (!rd_ok || !sum_ok)
    {
        fprintf(stderr, "  save: underrun=%d sum_ok=%d\n", !rd_ok, sum_ok);
        pw_print(t, !sum_ok ? "FAIL blob-sum" : "NOTE underrun");
    }
    std::string b_console;
    std::vector<uint32_t> b_mix;
    std::vector<Cpu> b_cpu;
    uint32_t b_canvas = 0, b_pc = 0;
    {
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
        g_console.clear();
        if (t.prog == 0)
            for (long i = 0; i < 20000000L
                             && g_console.find(DONE) == std::string::npos;
                 i++)
                step();
        if (t.prog == 0)
            for (long i = 0; i < 200000L; i++)
                step();
        else if (t.prog == 1)
            run_ticks((size_t)(b_at + n));
        else
            for (long i = 0; i < 400000L; i++)
                step();
        b_console = g_console;
        b_mix = g_mix;
        b_cpu = g_cpu;
        b_canvas = (uint32_t)MEM(prog__DOT__canvas_shadow);
        b_pc = (uint32_t)MEM(cpu__DOT__pc);
        if (!(int)MEM(resb) && t.prog != 0)
        {
            pw_print(t, "FAIL save-session-dead");
            pw_state("B");
        }
    }
    std::map<std::string, std::vector<uint8_t>> b_files = g_files;
    teardown();
    if (!sum_ok)
        return 103;

    if (t.refuse == 2)
    {
        uint32_t at = 8 + r.below(BLOB_BYTES / 4 - 16);
        blob[at * 4 + r.below(4)] ^= (uint8_t)(1u << r.below(8));
    }
    else if (t.refuse == 3)
    {
        blob_put(blob, 1, blob_get(blob, 1) - 1);
        blob_reseal(blob);
    }
    std::vector<uint8_t> file = wrap_blob(blob, 596 + (size_t)t.head_pad,
                                          52764);
    if (t.refuse == 4)
        file = wrap_blob({}, 8192, 0);

    g_files = card;
    int res = pw_wake(r, t, rom, file);
    long c_at = g_stop_tick;
    int bad = 0;
    if (t.refuse == 0 && t.seq != SEQ_NO_LOAD)
    {
        if (res != 0)
        {
            pw_print(t, res == 1 ? "FAIL good-blob-refused"
                                 : "FAIL load-stuck");
            pw_state("C");
            bad = 1;
        }
        else
        {
            /* The console is not cleared here: the wake's own passes
             * print what the 6502 had queued at the save. */
            long guard = 0;
            while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
                step();
            if (guard >= 20000000L)
            {
                pw_print(t, "FAIL wake-never-finished");
                pw_state("C");
                bad = 1;
            }
            if (t.prog == 0)
            {
                for (long i = 0; i < 20000000L
                                 && g_console.find(DONE) == std::string::npos;
                     i++)
                    step();
                for (long i = 0; i < 200000L; i++)
                    step();
                if (g_console != b_console)
                {
                    pw_print(t, "FAIL resume-console");
                    fprintf(stderr, "  B console=[%s]\n", b_console.c_str());
                    pw_state("C");
                    bad = 1;
                }
                else if (g_files != b_files)
                {
                    pw_print(t, "FAIL resume-card");
                    for (auto &f : b_files)
                        if (!g_files.count(f.first)
                            || g_files[f.first] != f.second)
                            fprintf(stderr, "  %s: %zu bytes, the session "
                                            "that slept on left %zu\n",
                                    f.first.c_str(),
                                    g_files.count(f.first)
                                        ? g_files[f.first].size() : 0,
                                    f.second.size());
                    for (auto &f : g_files)
                        if (!b_files.count(f.first))
                            fprintf(stderr, "  %s: only after the wake\n",
                                    f.first.c_str());
                    bad = 1;
                }
            }
            else if (t.prog == 1)
            {
                run_ticks((size_t)(c_at + n));
                if ((long)g_mix.size() < c_at + n || c_at < 0
                    || first_diff(b_mix, b_at, g_mix, c_at, n) >= 0
                    || first_cpu_diff(b_cpu, b_at, g_cpu, c_at, n) >= 0)
                {
                    pw_print(t, "FAIL resume-ticks");
                    pw_state("C");
                    bad = 1;
                }
            }
            else
            {
                long lim = t.prog == 3 ? 60000000L : 400000L;
                long took = 0;
                for (long i = 0; i < lim; i++, took++)
                {
                    step();
                    /* A game is given the time a start from the beginning
                     * takes to show its picture. */
                    if (t.prog == 3 && i > 3000000L && (int)MEM(resb)
                        && (uint32_t)MEM(prog__DOT__canvas_shadow) == b_canvas)
                        break;
                }
                if (t.prog == 3)
                    fprintf(stderr, "  the picture was back %ld steps after "
                                    "the wake\n", took);
                if (!(int)MEM(resb)
                    || (uint32_t)MEM(prog__DOT__canvas_shadow) != b_canvas)
                {
                    pw_print(t, "FAIL resume-picture");
                    pw_state("C");
                    bad = 1;
                }
            }
        }
    }
    else
    {
        if (res != (t.seq == SEQ_NO_LOAD ? 3 : 1))
        {
            pw_print(t, res == 0 ? "FAIL bad-blob-loaded" : "FAIL load-stuck");
            pw_state("C");
            bad = 1;
        }
        else
        {
            /* A blob refused, or one no load comes for: the program
             * starts over. */
            g_console.clear();
            bool again = false;
            /* The console says why, on its own canvas, and the line is
             * up for two seconds with the 6502 in reset before the
             * program starts. A blob with no load is given up two seconds
             * after its last word. */
            const long sec = 50400000L;
            std::string line = pw_line(t);
            long from = g_sys, shown = -1, started = -1;
            bool console = false;
            /* The firmware waits two seconds for a load that does not
             * come and shows its line for two more. A second is about
             * 125 million steps. */
            long lim = 700000000L;
            for (long i = 0; i < lim && !again; i++)
            {
                step();
                if (shown < 0 && (i & 0xFFFF) == 0
                    && pw_screen().find(line) != std::string::npos)
                {
                    shown = g_sys;
                    console = !(int)MEM(resb)
                              && !(uint32_t)MEM(prog__DOT__canvas_shadow);
                }
                if (shown >= 0 && started < 0 && (int)MEM(resb))
                    started = g_sys;
                if (t.prog == 0)
                    again = g_console.find(DONE) != std::string::npos;
                else if (t.prog == 1)
                    again = g_console.find('W') != std::string::npos;
                else
                    again = i > (t.prog == 3 ? 0L : 6000000L) && (int)MEM(resb)
                            && (uint32_t)MEM(prog__DOT__canvas_shadow) != 0;
            }
            if (!again)
            {
                pw_print(t, (int)MEM(resb) ? "FAIL refused-no-restart"
                                           : "FAIL refused-6502-stopped");
                pw_state("C");
                bad = 1;
            }
            else if (shown < 0 || !console || started - shown < sec * 19 / 10
                     || started - shown > sec * 3
                     || (t.seq == SEQ_NO_LOAD
                         && (shown - from < sec * 19 / 10
                             || shown - from > sec * 23 / 10)))
            {
                pw_print(t, "FAIL refused-line");
                fprintf(stderr, "  wanted [%s]: shown %.2f s after the wake, "
                                "on the console %d, the 6502 started %.2f s "
                                "later\n  screen [%s]\n", line.c_str(),
                        (double)(shown - from) / (double)sec, (int)console,
                        (double)(started - shown) / (double)sec,
                        pw_screen().substr(0, 400).c_str());
                bad = 1;
            }
            else if (!pw_program_runs(mark_canvas, b_pc))
            {
                pw_print(t, "FAIL refused-not-running");
                pw_state("C");
                bad = 1;
            }
        }
    }
    if (t.prog == 3)
        pw_state("C at the end");
    if (g_wild)
    {
        pw_print(t, "FAIL wild-write");
        bad = 1;
    }
    if (!bad)
        pw_print(t, "ok");
    teardown();
    return bad;
}

UTEST(pwake, round_trips)
{
    long seed = env_long("PWAKE_SEED", 1);
    long count = env_long("PWAKE_N", 1);
    int fails = 0;
    for (long k = 0; k < count; k++)
    {
        Trip t{};
        t.seed = seed + k;
        fails += pw_trip(t) != 0;
    }
    fprintf(stderr, "PWAKE done: %d of %ld failed\n", fails, count);
    ASSERT_EQ(0, fails);
}

/* The firmware reads SST_CTL three times in a pass of its main loop: in
 * wake_task, in the loop's own check about 110 clk_sys clocks later, and
 * in the next pass's audio task about 390 after that. This prints the
 * longest gap between two of the reads and the longest pass, which is
 * what PWAKE_SAVEWIN_D sweeps. */
UTEST(pwake, explore_window_share)
{
    int prog = (int)env_long("PWAKE_PROG", 2);
    std::vector<uint8_t> rom = pw_rom(prog);
    boot(rom);
    ASSERT_TRUE(pw_to_mark(prog));
    for (long i = 0; i < 300000; i++)
        step();
    long last = -1, first = -1, gap_max = 0, pass_max = 0, passes = 0;
    bool prev = false;
    while (passes < 300)
    {
        step();
        bool stb = dut->rootp->tb_pocket__DOT__core__DOT____Vcellinp__sst__stb;
        if (stb && !prev)
        {
            if (last >= 0 && g_sys - last > 500)
            {
                if (first >= 0 && g_sys - first > pass_max)
                    pass_max = g_sys - first;
                first = g_sys;
                passes++;
            }
            else if (last >= 0 && g_sys - last > gap_max)
                gap_max = g_sys - last;
            last = g_sys;
        }
        prev = stb;
    }
    fprintf(stderr, "%ld passes: at most %ld clocks between two reads and "
                    "%ld in a pass\n", passes, gap_max, pass_max);
    ASSERT_GT(gap_max, 0L);
    teardown();
}

/* One pinned trip. Every choice has a value here, and a case names the
 * ones it is about: the audio program, which does not start itself again
 * and is compared tick for tick, saved 100,000 steps after its mark, a
 * steady host that answers file requests, Reset Exit and then the blob
 * 1,000 steps later, while the firmware boots, and the load 1,000 steps
 * after the blob. */
static int pw_case(std::initializer_list<const char *> kv)
{
    static const char *const base[] = {
        "PWAKE_PROG", "1", "PWAKE_REFUSE", "0", "PWAKE_DRB", "1000",
        "PWAKE_SERVICE", "1", "PWAKE_DBL", "1000", "PWAKE_RMODE", "0",
        "PWAKE_RORDER", "0", "PWAKE_WMODE", "0", "PWAKE_WSHUFFLE", "0",
        "PWAKE_FWBRIDGE", "0", "PWAKE_SAVEAT", "100000", "PWAKE_SEQ", "0",
        "PWAKE_REPEAT", "0", "PWAKE_REPGAP", "0", "PWAKE_HEADPAD", "0",
        "PWAKE_SAVEWIN", "0", "PWAKE_SAVEWIN_D", "0", "PWAKE_SAVEOP", "0",
        "PWAKE_SAVEOP_K", "1", "PWAKE_BLOBOP", "0", "PWAKE_BLOBOP_K", "1",
        "PWAKE_LOADARM", "0"};
    std::vector<const char *> all(std::begin(base), std::end(base));
    all.insert(all.end(), kv.begin(), kv.end());
    for (size_t i = 0; i + 1 < all.size(); i += 2)
        setenv(all[i], all[i + 1], 1);
    Trip t{};
    t.seed = 1;
    int bad = pw_trip(t);
    for (size_t i = 0; i + 1 < all.size(); i += 2)
        unsetenv(all[i]);
    return bad;
}

/* The blob and the load in the order they are known or suspected to come
 * on a Pocket. A good blob resumes the program exactly in every one. */

/* The fresh firmware has staged the program and it runs when the blob
 * comes. */
UTEST(pwake, the_blob_and_the_load_after_the_program_started_resume)
{
    ASSERT_EQ(0, pw_case({"PWAKE_DRB", "5000000"}));
}

/* The blob is in before Reset Exit, and the load comes with the firmware
 * at its first instructions, in its boot, and long after it. */
UTEST(pwake, the_blob_in_reset_and_the_load_early_in_the_boot_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_DRB", "-1", "PWAKE_DBL", "0"}));
}

UTEST(pwake, the_blob_in_reset_and_the_load_during_the_boot_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_DRB", "-1", "PWAKE_DBL", "150000"}));
}

UTEST(pwake, the_blob_in_reset_and_the_load_after_the_boot_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_DRB", "-1", "PWAKE_DBL", "3300000"}));
}

/* The load arrives while the Pocket holds the core in reset, and the
 * Pocket waits for its ok before Reset Exit. */
UTEST(pwake, a_load_answered_in_reset_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "1"}));
}

UTEST(pwake, a_load_asked_for_in_reset_with_no_wait_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "2"}));
}

UTEST(pwake, a_load_asked_for_in_reset_before_its_blob_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "3"}));
}

/* The picture program, whose screen the firmware has to put back. */
UTEST(pwake, a_load_answered_in_reset_brings_the_picture_back)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "1", "PWAKE_PROG", "2"}));
}

/* The load asked for twice. */
UTEST(pwake, a_load_asked_for_twice_at_once_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REPEAT", "1"}));
}

UTEST(pwake, a_load_asked_for_twice_in_reset_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "1", "PWAKE_REPEAT", "1"}));
}

/* The second request comes once the engine is done, with the firmware
 * still putting back what the blob does not hold. */
UTEST(pwake, a_load_asked_for_again_as_the_firmware_wakes_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REPEAT", "2", "PWAKE_REPGAP", "50000000"}));
}

UTEST(pwake, a_second_load_after_the_wake_resumes_again)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REPEAT", "3"}));
}

/* The host answers no file request from the blob's first word until the
 * load is answered. The file program has its requests in flight, at boot
 * and with the program running. */
UTEST(pwake, a_host_that_answers_no_file_request_at_boot_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SERVICE", "0",
                          "PWAKE_SAVEAT", "20000"}));
}

UTEST(pwake, a_host_that_answers_no_file_request_later_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SERVICE", "0",
                          "PWAKE_DRB", "5000000", "PWAKE_SAVEAT", "20000"}));
}

/* A refused blob. The program starts over: the 6502 runs it and the
 * screen is its own. The firmware saw these blobs before it staged
 * anything. */
UTEST(pwake, another_build_refused_in_reset_starts_the_program)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "1", "PWAKE_REFUSE", "1"}));
}

UTEST(pwake, another_version_refused_in_reset_starts_the_picture)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "1", "PWAKE_REFUSE", "3",
                          "PWAKE_PROG", "2"}));
}

UTEST(pwake, a_corrupt_word_refused_during_the_boot_starts_the_program)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REFUSE", "2"}));
}

/* The fresh firmware has staged the program and has stopped it for the
 * blob, and the blob is refused. The console says which check refused it
 * and the program starts over. */
UTEST(pwake, a_refused_wake_after_staging_starts_the_program_over)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REFUSE", "1", "PWAKE_DRB", "5000000"}));
}

UTEST(pwake, another_version_refused_after_staging_starts_the_picture_over)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REFUSE", "3", "PWAKE_DRB", "5000000",
                          "PWAKE_PROG", "2"}));
}

UTEST(pwake, a_corrupt_word_refused_after_staging_starts_the_program_over)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REFUSE", "2", "PWAKE_DRB", "5000000"}));
}

UTEST(pwake, a_file_with_no_blob_refused_after_staging_starts_the_program_over)
{
    ASSERT_EQ(0, pw_case({"PWAKE_REFUSE", "4", "PWAKE_DRB", "5000000"}));
}

/* A blob and no load. Two seconds after the blob's last word the firmware
 * gives up on it, the console says so and the program starts. */
UTEST(pwake, a_blob_in_reset_with_no_load_starts_the_program)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "4", "PWAKE_DRB", "-1"}));
}

UTEST(pwake, a_blob_with_no_load_starts_the_program_over)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "4", "PWAKE_DRB", "5000000"}));
}

/* The load comes after the firmware has given the blob up and said so,
 * and it still finds the blob: the program resumes. */
UTEST(pwake, a_load_after_the_blob_was_given_up_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_DRB", "-1", "PWAKE_DBL", "325000000"}));
}

/* The save halts the firmware between the wake task's read of SST_CTL and
 * the main loop's checks for a new ROM, and the wake's MMIO_SLOT still
 * holds the ROM's length because the fresh firmware saw the blob before it
 * staged. The program resumes; a firmware that acted on MMIO_SLOT there
 * would start it over. The save is asked for at wake_task's read and so
 * many clocks after it, and the soft CPU halts 6 clocks after it is
 * asked. The loop's checks come about 110 clocks after wake_task's
 * read. */
UTEST(pwake, a_wake_halted_before_the_restage_checks_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SAVEWIN", "1"}));
}

UTEST(pwake, a_wake_halted_after_the_wake_task_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SAVEWIN", "1", "PWAKE_SAVEWIN_D", "30",
                          "PWAKE_SEQ", "1"}));
    ASSERT_EQ(0, pw_case({"PWAKE_SAVEWIN", "1", "PWAKE_SAVEWIN_D", "60",
                          "PWAKE_SEQ", "2"}));
    ASSERT_EQ(0, pw_case({"PWAKE_SAVEWIN", "1", "PWAKE_SAVEWIN_D", "90",
                          "PWAKE_SEQ", "1"}));
}

UTEST(pwake, a_wake_halted_at_the_restage_checks_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SAVEWIN", "1", "PWAKE_SAVEWIN_D", "96",
                          "PWAKE_SEQ", "2"}));
    ASSERT_EQ(0, pw_case({"PWAKE_SAVEWIN", "1", "PWAKE_SAVEWIN_D", "102",
                          "PWAKE_SEQ", "1"}));
    ASSERT_EQ(0, pw_case({"PWAKE_SAVEWIN", "1", "PWAKE_SAVEWIN_D", "108"}));
}

/* The save halts the firmware inside a file call of the 6502's, and the
 * call ends after the wake as it would have: the program prints what it
 * read back, the card holds what it wrote, and no write went out with
 * another session's registers. */

/* The write's slot is written and the write not yet issued: saved before
 * its data is in the window, and saved with only its length and the
 * command to come, which then reach a pocket_file that holds none of the
 * rest. */
UTEST(pwake, a_file_write_about_to_be_issued_at_the_sleep_succeeds)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x18", "PWAKE_SAVEOP_K", "3",
                          "PWAKE_SEQ", "1"}));
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x18", "PWAKE_SAVEOP_K", "5",
                          "PWAKE_SEQ", "1"}));
}

/* The same save, and on the wake the fresh firmware has run the program
 * into its first open, where the host leaves it while it writes the blob.
 * The load then halts that firmware with a command's slot written, so the
 * write's length and command from the blob's firmware meet a pocket_file
 * that another session armed. The write is not run. */
UTEST(pwake, a_file_write_meets_no_command_the_fresh_firmware_armed)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x18", "PWAKE_SAVEOP_K", "5",
                          "PWAKE_BLOBOP", "0x14", "PWAKE_BLOBOP_K", "2",
                          "PWAKE_SERVICE", "2", "PWAKE_LOADARM", "1"}));
}

UTEST(pwake, a_file_write_issued_at_the_sleep_succeeds)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x18", "PWAKE_SAVEOP_K", "7",
                          "PWAKE_DRB", "5000000"}));
}

UTEST(pwake, a_file_write_growing_its_file_at_the_sleep_succeeds)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x18", "PWAKE_SAVEOP_K", "2",
                          "PWAKE_SEQ", "2"}));
}

/* The open that creates the file: the save comes as the create is issued,
 * and once it is done and the open that checks it is next. */
UTEST(pwake, a_file_open_creating_its_file_at_the_sleep_succeeds)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x14", "PWAKE_SAVEOP_K", "4",
                          "PWAKE_SEQ", "1"}));
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x14", "PWAKE_SAVEOP_K", "5"}));
}

UTEST(pwake, a_file_read_issued_at_the_sleep_succeeds)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x16", "PWAKE_SAVEOP_K", "5",
                          "PWAKE_SEQ", "1"}));
}

UTEST(pwake, a_file_close_flushing_at_the_sleep_succeeds)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x15", "PWAKE_SAVEOP_K", "2",
                          "PWAKE_SEQ", "1"}));
}

/* The SDRAM controller counts the chip's 200 us of power-up wait from
 * the PLL's lock, which the bench drives as rst_n. */
UTEST(pwake, the_sdram_waits_for_the_pll)
{
    dut = new Vtb_pocket;
    a_next = s_next = g_sys = 0;
    dut->rst_n = 0;
    dut->arst_n = 0;
    for (int i = 0; i < 60000; i++)
        tick();
    ASSERT_FALSE((int)dut->tb_pocket_ready);
    dut->rst_n = 1;
    dut->arst_n = 1;
    long from = g_sys;
    for (int i = 0; i < 60000 && !dut->tb_pocket_ready; i++)
        tick();
    ASSERT_TRUE((int)dut->tb_pocket_ready);
    /* 200 us is 10,080 clocks of clk_sys. */
    ASSERT_GE(g_sys - from, 10080L);
    teardown();
}
