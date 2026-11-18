/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The Pocket's sleep and wake on the whole core: the firmware makes the
 * savestate in the blob window, the host reads it back, and a later
 * session loads it. A run is held to the same session run on without a
 * save, which is a copy of the model from just before the save.
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "pocket_version.h"
#include "tb_snap.h"
#include "tb_stage.h"
#include "utest.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

static Vtb_pocket *dut;

#define MEM(f) (dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__##f)

/* What the Pocket does with savestates, and where each piece comes from.
 * The bench holds the core to these, and nothing else in this file
 * guesses. Where a Pocket disagrees, suspect the guesses first.
 *
 * The sources: Analogue's docs,
 * https://www.analogue.co/developer/docs/openfpga/host-target-commands;
 * Analogue's template, vendor/openfpga,
 * https://github.com/open-fpga/core-template; the Spacewar core,
 * https://github.com/spacemen3/PDP-1, Analogue's own example with Sleep
 * and Memories, taken as canonical; a community PlayStation core,
 * https://github.com/GuardianSoul/openfpga-PlayStation; the owner's Pocket.
 *
 * - A save answers busy, then ok on a later query. The result codes are
 *   the docs'. Busy while the save is made is from the template's
 *   core_bridge_cmd.v notes, and Spacewar answers busy while it waits up
 *   to two video frames. That the host queries on through busy follows;
 *   nobody has watched it.
 * - A load into the running machine answers busy, then ok. The same
 *   sources, for the load.
 * - A wake: a fresh configuration of the core on the card the sleeping
 *   session left. The ROM slot already holds the ROM the host loaded, as
 *   it loads every data slot before Reset Exit: Analogue's docs on data
 *   slots, and measured on the owner's Pocket. The blob is written into
 *   the window, then Reset Exit, then the load is asked for. The order is
 *   inferred: the owner's Pocket left a wake's load unanswered while the
 *   firmware's rows ran (below), and this core answers ok at once a load
 *   asked for in reset. a_bad_savestate_stops_the_machine asks in reset,
 *   which the PlayStation core reports can happen; Spacewar answers a load
 *   there, its savestate logic running on its own power-on reset.
 * - A load writes the savestate at offset 0 of the window, with nothing
 *   in front: measured on the owner's Pocket, and it agrees with Spacewar,
 *   which takes the host's writes straight in at savestate_addr. The
 *   Memories file on the card has a 596-byte header and a thumbnail
 *   around the savestate, and neither comes to the core.
 * - While a load request is unanswered the host serves no file command:
 *   measured on the owner's Pocket, where an Open File sent then got no
 *   reply until the bridge's deadline. What the host does with such a
 *   command once the load is answered is not known. The bench never
 *   answers it, counts it, and ends the load there rather than wait out
 *   the deadline.
 * - The host serves no file reads while it writes a Memory: reported by
 *   the PlayStation core. The bench does not model it; the firmware
 *   saves only with no file command in flight.
 * - A save refused with err is the end of it here. What the Pocket then
 *   does with its sleep is not known. */
enum { RES_IDLE = 0, RES_BUSY = 1, RES_OK = 2, RES_ERR = 3 };
static const int ANSWER_SAVE = RES_BUSY;
static const int ANSWER_LOAD = RES_BUSY;
static const int ANSWER_LOAD_IN_RESET = RES_OK;

/* core_top's savestate_addr and savestate_size. */
#define BLOB_BRIDGE 0x03EC1000u
#define BLOB_BYTES POCKET_SST_BYTES

enum { REQ_NONE = 0, REQ_OPEN, REQ_GETFILE, REQ_READ, REQ_WRITE };

/* The 6502's clock at a psg_tick: its PC and bus address, its PHI2
 * accumulator and VIA T1, so a machine that comes back a clock off shows
 * even where the mix does not. */
struct Cpu
{
    uint32_t pc, addr, acc, t1;
    bool operator!=(const Cpu &o) const
    {
        return pc != o.pc || addr != o.addr || acc != o.acc || t1 != o.t1;
    }
};

/* The bench's side of a session, which a copy of the model keeps with it,
 * so io() lists every field. */
struct Bench
{
    long a_next = 0, s_next = 0, sys = 0;
    uint32_t dt_pipe[2] = {};
    uint32_t dt[64] = {};

    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> dirs;
    std::string bound[16];
    /* The bytes the 6502 writes to the RIA's TX register. */
    std::string console;
    /* Slot Reads and Slot Writes for a slot the host has bound to nothing.
     * What a Pocket answers to one is not known, and the firmware has no
     * reason to send one. */
    int stray = 0;
    /* loading is high from a load request until the host has seen it
     * answered. unserved counts the file commands sent in that time, and
     * rom_reads the Slot Reads of slot 0, the ROM slot. */
    bool loading = false;
    int unserved = 0, rom_reads = 0;

    int req = REQ_NONE, servicing = 0;
    int prev_r = 0, prev_w = 0, prev_o = 0, prev_g = 0;

    /* The mix and the 6502's clock at each rising psg_tick, and the result
     * of each API call as the 6502 takes it. */
    std::vector<uint32_t> mix;
    std::vector<Cpu> cpu;
    std::vector<uint32_t> api;
    int tick_prev = 0;
    bool api_prev = false;
    /* Where the records stood when the machine was last held, and when it
     * was last let go: no tick passes in between. */
    bool held_prev = false;
    long hold_tick = -1, hold_api = 0, hold_con = 0;
    long free_tick = -1, free_api = 0, free_con = 0;
    /* clk_sys at the last hold, and how long the last hold lasted. */
    long hold_sys = 0, held_sys = 0;

    template <typename S> void io(S &s)
    {
        tb_snap_io(s, a_next);
        tb_snap_io(s, s_next);
        tb_snap_io(s, sys);
        tb_snap_io(s, dt_pipe);
        tb_snap_io(s, dt);
        tb_snap_io(s, files);
        tb_snap_io(s, dirs);
        tb_snap_io(s, bound);
        tb_snap_io(s, console);
        tb_snap_io(s, stray);
        tb_snap_io(s, loading);
        tb_snap_io(s, unserved);
        tb_snap_io(s, rom_reads);
        tb_snap_io(s, req);
        tb_snap_io(s, servicing);
        tb_snap_io(s, prev_r);
        tb_snap_io(s, prev_w);
        tb_snap_io(s, prev_o);
        tb_snap_io(s, prev_g);
        tb_snap_io(s, mix);
        tb_snap_io(s, cpu);
        tb_snap_io(s, api);
        tb_snap_io(s, tick_prev);
        tb_snap_io(s, api_prev);
        tb_snap_io(s, held_prev);
        tb_snap_io(s, hold_tick);
        tb_snap_io(s, hold_api);
        tb_snap_io(s, hold_con);
        tb_snap_io(s, free_tick);
        tb_snap_io(s, free_api);
        tb_snap_io(s, free_con);
        tb_snap_io(s, hold_sys);
        tb_snap_io(s, held_sys);
    }
};
static Bench tb;

/* The 6502's API call as the RIA window shows it: $FFF2 holds BRA's -2
 * while a call is in flight, and $FFF4 and $FFF6 its result once it is
 * not. */
static bool api_busy()
{
    return MEM(ria__DOT__regs)[0x12] == 0xFE;
}

/* Called on every edge, so no tick is missed. */
static void observe()
{
    auto *r = dut->rootp;
    int t = MEM(psg_tick);
    if (t && !tb.tick_prev)
    {
        tb.mix.push_back((uint32_t)(uint16_t)r->tb_pocket__DOT__core__DOT__aud_l << 16
                         | (uint16_t)r->tb_pocket__DOT__core__DOT__aud_r);
        tb.cpu.push_back({(uint32_t)MEM(cpu__DOT__pc), (uint32_t)MEM(cpu_addr),
                          (uint32_t)MEM(phi2__DOT__acc),
                          (uint32_t)MEM(via__DOT__t1_counter)});
    }
    tb.tick_prev = t;
    bool busy = api_busy();
    if (tb.api_prev && !busy)
        tb.api.push_back(MEM(ria__DOT__regs)[0x14] | MEM(ria__DOT__regs)[0x16] << 8);
    tb.api_prev = busy;
    bool held = MEM(held);
    if (held && !tb.held_prev)
    {
        tb.hold_tick = (long)tb.mix.size();
        tb.hold_api = (long)tb.api.size();
        tb.hold_con = (long)tb.console.size();
        tb.hold_sys = tb.sys;
    }
    if (!held && tb.held_prev)
    {
        tb.free_tick = (long)tb.mix.size();
        tb.free_api = (long)tb.api.size();
        tb.free_con = (long)tb.console.size();
        tb.held_sys = tb.sys - tb.hold_sys;
    }
    tb.held_prev = held;
}

static void tick()
{
    long next = tb.a_next < tb.s_next ? tb.a_next : tb.s_next;
    bool sedge = next == tb.s_next;
    bool aedge = next == tb.a_next;
    if (sedge)
    {
        dut->clk_sys = 1;
        if ((tb.sys & 1) == 0)
            dut->clk_vid = 1;
    }
    if (aedge)
    {
        /* mf_datatable registers both the address and the output of port A
         * on CLOCK0, so datatable_q follows the address by two clk_74a
         * cycles. */
        dut->datatable_q = tb.dt_pipe[1];
        tb.dt_pipe[1] = tb.dt_pipe[0];
        tb.dt_pipe[0] = tb.dt[dut->tb_pocket_dt_addr & 63];
        dut->clk_74a = 1;
    }
    dut->eval();
    if (sedge && dut->tb_pocket_tx_valid)
        tb.console += (char)dut->tb_pocket_tx_data;
    /* pocket_file raises each request line for a single clk_74a cycle,
     * and core_bridge_cmd queues a command on the rising edge of its
     * request line, so the lines are sampled on every tick(). */
    {
        int r = dut->tb_pocket_ds_read, w = dut->tb_pocket_ds_write,
            o = dut->tb_pocket_ds_openfile, g = dut->tb_pocket_ds_getfile;
        if (o && !tb.prev_o)
            tb.req = REQ_OPEN;
        else if (g && !tb.prev_g)
            tb.req = REQ_GETFILE;
        else if (r && !tb.prev_r)
            tb.req = REQ_READ;
        else if (w && !tb.prev_w)
            tb.req = REQ_WRITE;
        tb.prev_r = r;
        tb.prev_w = w;
        tb.prev_o = o;
        tb.prev_g = g;
    }
    if (sedge)
    {
        dut->clk_sys = 0;
        dut->clk_vid = 0;
        tb.s_next += 330;
        tb.sys++;
    }
    if (aedge)
    {
        dut->clk_74a = 0;
        tb.a_next += 224;
    }
    dut->eval();
    observe();
}

static void a_edge()
{
    long t = tb.a_next;
    while (tb.a_next == t)
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

/* io_bridge_peripheral.v buffers reads by one word, so the host puts the
 * next word's address on bridge_addr before it takes this word, and the
 * core has until the next read strobe to serve it. The gap is about one
 * microsecond of clk_74a, shorter than the 88 cycles io_bridge_peripheral.v
 * gives as the fastest host access. */
static uint32_t host_read(uint32_t addr)
{
    dut->bridge_addr = addr;
    a_edge();
    dut->bridge_rd = 1;
    a_edge();
    dut->bridge_rd = 0;
    dut->bridge_addr = addr + 4;
    for (int k = 0; k < 74; k++)
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
    tb.dt[slot * 2] = slot;
    tb.dt[slot * 2 + 1] = size;
}

static void target_answer(int err)
{
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = err;
    for (int k = 0; k < 4; k++)
        a_edge();
}

static void do_openfile()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint8_t param[264];
    host_get_bytes(dut->tb_pocket_param_struct, param, sizeof param);
    std::string key((const char *)param);
    /* The host does not resolve relative names. */
    if (key.empty() || key[0] != '/')
        return target_answer(4); /* malformed path */
    std::string parent = key.substr(0, key.rfind('/'));
    if (parent.empty())
        parent = "/";
    uint32_t flags = ((uint32_t)param[256] << 24) | ((uint32_t)param[257] << 16)
                     | ((uint32_t)param[258] << 8) | (uint32_t)param[259];
    uint32_t size = ((uint32_t)param[260] << 24) | ((uint32_t)param[261] << 16)
                    | ((uint32_t)param[262] << 8) | (uint32_t)param[263];
    bool created = false;
    auto it = tb.files.find(key);
    if (it == tb.files.end())
    {
        if (!(flags & 1))
            return target_answer(3); /* file not found */
        /* The host creates a file only when both the create bit and the
         * resize bit are set, and only into a folder that exists. Otherwise
         * Open File returns 1 and makes nothing. */
        if (!(flags & 2) || !tb.dirs.count(parent))
            return target_answer(1);
        it = tb.files.emplace(key, std::vector<uint8_t>()).first;
        created = true;
    }
    /* What the host puts in the bytes a resize adds is unrecorded. */
    if (flags & 2)
        it->second.resize(size, 0xA5);
    tb.bound[slot] = key;
    dt_set(slot, (uint32_t)it->second.size());
    target_answer(created ? 1 : 0);
}

static bool slot_bound(uint32_t slot)
{
    if (slot < 16 && !tb.bound[slot].empty())
        return true;
    tb.stray++;
    target_answer(1);
    return false;
}

static void do_slotread()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t off = dut->tb_pocket_ds_slotoffset;
    uint32_t len = dut->tb_pocket_ds_length;
    uint32_t at = dut->tb_pocket_ds_bridgeaddr;
    if (slot == 0)
        tb.rom_reads++;
    if (!slot_bound(slot))
        return;
    std::vector<uint8_t> &f = tb.files[tb.bound[slot]];
    std::vector<uint8_t> chunk(len, 0);
    for (uint32_t i = 0; i < len && off + i < f.size(); i++)
        chunk[i] = f[off + i];
    host_put_bytes(at, chunk.data(), chunk.size());
    target_answer(0);
}

static void do_slotwrite()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t off = dut->tb_pocket_ds_slotoffset;
    uint32_t len = dut->tb_pocket_ds_length;
    uint32_t at = dut->tb_pocket_ds_bridgeaddr;
    if (!slot_bound(slot))
        return;
    std::vector<uint8_t> chunk(len, 0);
    host_get_bytes(at, chunk.data(), chunk.size());
    std::vector<uint8_t> &f = tb.files[tb.bound[slot]];
    if (f.size() < off + len)
        f.resize(off + len, 0);
    for (uint32_t i = 0; i < len; i++)
        f[off + i] = chunk[i];
    dt_set(slot, (uint32_t)f.size());
    target_answer(0);
}

static void do_getfile()
{
    dut->target_dataslot_done = 0;
    const std::string &name = tb.bound[dut->tb_pocket_ds_id];
    std::vector<uint8_t> resp(256, 0);
    for (size_t i = 0; i < name.size() && i + 1 < resp.size(); i++)
        resp[i] = (uint8_t)name[i];
    host_put_bytes(dut->tb_pocket_resp_struct, resp.data(), resp.size());
    target_answer(0);
}

static void step()
{
    tick();
    if (!tb.req || tb.servicing)
        return;
    int req = tb.req;
    tb.req = REQ_NONE;
    if (tb.loading)
    {
        tb.unserved++;
        return;
    }
    tb.servicing = 1;
    switch (req)
    {
    case REQ_OPEN: do_openfile(); break;
    case REQ_GETFILE: do_getfile(); break;
    case REQ_READ: do_slotread(); break;
    default: do_slotwrite(); break;
    }
    tb.servicing = 0;
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

/* The card a session runs on. */
struct Card
{
    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> dirs = {"/", "/Assets", "/Assets/rp6502",
                                  "/Assets/rp6502/common", "/Saves",
                                  "/Saves/rp6502", "/Saves/rp6502/common"};
};

/* A fresh configuration on card with rom in slot 0, held in reset. */
static void boot_held(const std::vector<uint8_t> &rom, const Card &card)
{
    dut = new Vtb_pocket;
    tb = Bench{};
    tb.files = card.files;
    tb.dirs = card.dirs;
    tb.bound[0] = "/Assets/rp6502/common/sleep.rp6502";
    tb.files[tb.bound[0]] = rom;

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
    tb_stage_poke(dut->rootp->tb_pocket__DOT__chip__DOT__mem,
                  TB_STAGE_MEM_BASE, tb_stage_firmware(SW_BIN));
    dut->rst_n = 1;
    dut->arst_n = 1;
    /* pocket_sdram takes no write until its power-up wait is over. */
    for (int i = 0; i < 40000 && !dut->tb_pocket_ready; i++)
        tick();
    host_put_bytes(TB_STAGE_ROM_BASE, rom.data(), rom.size());
    dt_set(0, (uint32_t)rom.size());
    dut->rtc_epoch = 1000000000u;
    dut->rtc_valid = 1;
    host_write(0x1000000Cu, 5);
    host_write(0x10000010u, 30);
    host_write(0x10000014u, 0);
}

/* The host's 0x008F and Reset Exit. */
static void release_reset()
{
    dut->datatable_q = tb.dt[1];
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
}

static void boot(const std::vector<uint8_t> &rom, const Card &card)
{
    boot_held(rom, card);
    release_reset();
    for (int i = 0; i < 4000; i++)
        step();
}

static void teardown()
{
    dut->final();
    delete dut;
    dut = nullptr;
}

static void a_step()
{
    long t = tb.a_next;
    while (tb.a_next == t)
        step();
}

/* A savestate command as core_bridge_cmd runs it, which test_psst.cpp
 * holds the real one to. A query's result is the status lines on one
 * clk_74a edge. A request raises its line and holds it until the edge
 * that sees the ack and two more, and its result is the status lines on
 * the ack's edge. Returns -1 when no ack comes. In reset the bench answers
 * no file requests, as the firmware makes none. */
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
    return acked ? code : -1;
}

/* Asks for a save and steps until it is answered. Returns the answer. */
static int save(void)
{
    if (sst_cmd(false, true, true) != ANSWER_SAVE)
        return -1;
    for (long i = 0; i < 100000000L && dut->tb_pocket_savestate_start_busy; i++)
        step();
    return sst_cmd(false, false, true);
}

/* Asks for a load and queries it until it is answered, serving no file
 * command in between. True when the request answers first and the last
 * query ok, with no file command sent while the load was unanswered. */
static bool load(int first)
{
    tb.loading = true;
    int got = sst_cmd(true, true, true);
    for (long i = 0; i < 100000000L && got == first && !tb.unserved
                    && dut->tb_pocket_savestate_load_busy;
         i++)
        step();
    int last = got == first && !tb.unserved ? sst_cmd(true, false, true) : -1;
    tb.loading = false;
    if (last != RES_OK || tb.unserved)
        fprintf(stderr, "  the load answered %d, then %d, with %d file "
                        "commands sent before it was answered\n",
                got, last, tb.unserved);
    return last == RES_OK && !tb.unserved;
}

/* The window as the host reads it, BLOB_BYTES from its start. The bench
 * reads a few words over the bridge and holds them to the SDRAM, then
 * takes the rest from the SDRAM, which saves the host's minutes of reads.
 * test_psst.cpp reads the whole window over the bridge. */
static bool read_blob(std::vector<uint8_t> &blob)
{
    auto &mem = dut->rootp->tb_pocket__DOT__chip__DOT__mem;
    blob.resize(BLOB_BYTES);
    for (uint32_t i = 0; i < BLOB_BYTES; i++)
        blob[i] = (uint8_t)(mem[(BLOB_BRIDGE + i) >> 1] >> (8 * (i & 1)));
    for (uint32_t i = 0; i < BLOB_BYTES; i += BLOB_BYTES / 8 - 4)
    {
        uint32_t w = host_read(BLOB_BRIDGE + i);
        for (uint32_t k = 0; k < 4; k++)
            if (blob[i + k] != (uint8_t)(w >> (24 - 8 * k)))
                return false;
    }
    return !dut->tb_pocket_savestate_start_err;
}

/* The blob at offset 0 of the window, where the host's writes put it.
 * Streaming it over the bridge would cost minutes, so it goes straight
 * into the SDRAM, and its first word goes again over the bridge, which is
 * how the core sees a blob arrive. test_pbridge.cpp holds the bridge's
 * writes to the SDRAM. */
static void put_blob(const std::vector<uint8_t> &blob)
{
    tb_stage_poke(dut->rootp->tb_pocket__DOT__chip__DOT__mem, BLOB_BRIDGE, blob);
    host_put_bytes(BLOB_BRIDGE, blob.data(), 4);
}

/* What a session did from a point on: the records from where they stood
 * then. */
struct Rec
{
    std::vector<uint32_t> mix, api;
    std::vector<Cpu> cpu;
    std::string console;
    uint32_t drops = 0, level = 0;
    int rom_reads = 0;
};

/* Steps until ticks psg_ticks and calls API results have passed since the
 * point, then takes the records from it. */
static Rec record(long tick, long call, long con, long ticks, long calls)
{
    for (long i = 0; i < 400000000L
                    && ((long)tb.mix.size() < tick + ticks
                        || (long)tb.api.size() < call + calls);
         i++)
        step();
    Rec r;
    r.mix.assign(tb.mix.begin() + tick, tb.mix.end());
    r.cpu.assign(tb.cpu.begin() + tick, tb.cpu.end());
    r.api.assign(tb.api.begin() + call, tb.api.end());
    r.console = tb.console.substr((size_t)con);
    r.drops = (uint32_t)MEM(opl_drops);
    r.level = (uint32_t)MEM(pcm_level);
    r.rom_reads = tb.rom_reads;
    return r;
}

/* The first index at which got differs from want over n, or -1. */
template <typename T>
static long first_diff(const std::vector<T> &want, const std::vector<T> &got,
                       long n)
{
    if ((long)want.size() < n || (long)got.size() < n)
        return 0;
    for (long i = 0; i < n; i++)
        if (want[(size_t)i] != got[(size_t)i])
            return i;
    return -1;
}

/* A session saved and run on (b), the same session run on unsaved (a),
 * a wake from b's savestate (c) and a load of it into a's running machine
 * once a has run its window (d), each from the point the machine stood
 * still. */
struct Trip
{
    Rec a, b, c, d;
};

/* The bench's clk_sys: 330 steps a cycle to clk_74a's 224. */
#define SYS_HZ 50400000.0

static void held_report(const char *what)
{
    fprintf(stderr, "  %s held the machine %.3f s\n", what,
            (double)tb.held_sys / SYS_HZ);
}

/* Boots rom on card, steps until at() is true, saves, and records ticks
 * and calls from there in each session that wake and running ask for. */
template <typename At>
static bool round_trip(const std::vector<uint8_t> &rom, const Card &card, At at,
                       long ticks, long calls, bool wake, bool running, Trip &t)
{
    boot(rom, card);
    for (long i = 0; i < 100000000L && !at(); i++)
        step();
    if (!at())
        return false;
    std::vector<uint8_t> fork = tb_snap_take(*dut, tb);

    tb.hold_tick = -1;
    if (save() != RES_OK || tb.hold_tick < 0)
        return false;
    held_report("the save");
    std::vector<uint8_t> blob;
    if (!read_blob(blob))
        return false;
    /* The card the sleeping session leaves. */
    Card slept;
    slept.files = tb.files;
    slept.dirs = tb.dirs;
    long tick = tb.hold_tick, call = tb.hold_api, con = tb.hold_con;
    t.b = record(tick, call, con, ticks, calls);
    if (tb.stray)
        return false;

    tb_snap_load(*dut, fork, tb);
    t.a = record(tick, call, con, ticks, calls);
    if (running)
    {
        tb.free_tick = -1;
        put_blob(blob);
        if (!load(ANSWER_LOAD))
            return false;
        for (long i = 0; i < 100000000L && tb.free_tick < 0; i++)
            step();
        if (tb.free_tick < 0)
            return false;
        held_report("the load");
        t.d = record(tb.free_tick, tb.free_api, tb.free_con, ticks, calls);
    }
    if (tb.stray)
        return false;
    teardown();

    if (wake)
    {
        boot_held(rom, slept);
        put_blob(blob);
        release_reset();
        if (!load(ANSWER_LOAD))
            return false;
        for (long i = 0; i < 100000000L && tb.free_tick < 0; i++)
            step();
        if (tb.free_tick < 0)
            return false;
        held_report("the wake");
        t.c = record(tb.free_tick, tb.free_api, tb.free_con, ticks, calls);
        if (tb.stray)
            return false;
        teardown();
    }
    return true;
}

UTEST_MAIN();

/* The audio program: the PSG, the OPL, PCM three quarters full and a VIA
 * timer, the 6502 writing them through RW0 and making no API call. The
 * save comes 2 ms after W, and a tick is a sample, 1050 clocks. The
 * window is 1,200 ticks, 25 ms. */
#define AUD_WINDOW 1200L

static bool aud_trip(const char *path, bool wake, bool running, Trip &t)
{
    std::vector<uint8_t> rom = read_file(path);
    long after = -1;
    return !rom.empty()
           && round_trip(rom, Card{}, [&] {
                  if (after < 0 && tb.console.find('W') != std::string::npos)
                      after = (long)tb.mix.size();
                  return after >= 0 && (long)tb.mix.size() >= after + 100;
              }, AUD_WINDOW, 0, wake, running, t);
}

static void aud_check(int *utest_result, const Rec &want, const Rec &got)
{
    long nz = 0;
    for (long i = 0; i < AUD_WINDOW; i++)
        nz += want.mix[(size_t)i] != 0;
    ASSERT_GT(nz, AUD_WINDOW / 2);
    ASSERT_EQ(-1L, first_diff(want.cpu, got.cpu, AUD_WINDOW));
    ASSERT_EQ(-1L, first_diff(want.mix, got.mix, AUD_WINDOW));
    ASSERT_EQ(want.drops, got.drops);
    ASSERT_EQ(want.level, got.level);
}

/* A save leaves the session as if none was made, and a wake into a fresh
 * boot runs on from it tick for tick: the 6502's clock, the mix, the OPL's
 * drops and PCM's level. */
UTEST(psleep, a_wake_plays_on_as_if_never_saved)
{
    Trip t;
    ASSERT_TRUE(aud_trip(AUD_SST_ROM, true, false, t));
    aud_check(utest_result, t.a, t.b);
    aud_check(utest_result, t.a, t.c);
}

/* The same with the PCM written as an odd length, so the save is made
 * partway into a word of the FIFO. */
UTEST(psleep, a_wake_after_an_odd_pcm_write_plays_on)
{
    Trip t;
    ASSERT_TRUE(aud_trip(AUD_SST_ODD_ROM, true, false, t));
    aud_check(utest_result, t.a, t.b);
    aud_check(utest_result, t.a, t.c);
}

/* A load into the running machine is the same. */
UTEST(psleep, a_load_into_the_running_machine_plays_on)
{
    Trip t;
    ASSERT_TRUE(aud_trip(AUD_SST_ROM, false, true, t));
    aud_check(utest_result, t.a, t.b);
    aud_check(utest_result, t.a, t.d);
}

/* The file program reads probe.dat 16 bytes a call, rewinding at its end,
 * and prints a count. A wake reopens the file where it was: each call
 * after it returns what it would have, and the count runs on. */
UTEST(psleep, an_open_file_reads_on_after_a_wake)
{
    std::vector<uint8_t> rom = read_file(SLEEPFILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    Card card;
    for (int u = 0; u < 64; u++)
        for (const char *c = "0123456789ABCDEF"; *c; c++)
            card.files["/Saves/rp6502/common/probe.dat"].push_back((uint8_t)*c);
    const long calls = 100;
    Trip t;
    ASSERT_TRUE(round_trip(rom, card, [] { return tb.api.size() >= 40; }, 0,
                           calls, true, false, t));
    ASSERT_EQ(-1L, first_diff(t.a.api, t.b.api, calls));
    ASSERT_EQ(-1L, first_diff(t.a.api, t.c.api, calls));
    size_t n = std::min(t.a.console.size(), t.c.console.size());
    ASSERT_GT(n, 64u);
    ASSERT_TRUE(t.a.console.compare(0, n, t.c.console, 0, n) == 0);
}

/* The asset program reads its ROM's asset over and over through ROM:. A
 * wake opens the ROM again from the slot the host loaded, reading none of
 * it off the card, and the reads after it return what they would have. */
UTEST(psleep, a_rom_asset_reads_on_after_a_wake)
{
    std::vector<uint8_t> rom = read_file(SLEEPASSET_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long calls = 60;
    Trip t;
    ASSERT_TRUE(round_trip(rom, Card{}, [] { return tb.api.size() >= 30; }, 0,
                           calls, true, false, t));
    ASSERT_EQ(-1L, first_diff(t.a.api, t.b.api, calls));
    ASSERT_EQ(-1L, first_diff(t.a.api, t.c.api, calls));
    ASSERT_EQ(0, t.c.rom_reads);
    ASSERT_TRUE(t.c.console.find("***") == std::string::npos);
    ASSERT_TRUE(t.c.console.find("FAILED") == std::string::npos);
}

/* The clock program reads the program's clock in milliseconds over and
 * over. After a wake it reads what the session that saved read at the
 * same call, within the millisecond a call can straddle. */
UTEST(psleep, the_program_clock_comes_back_after_a_wake)
{
    std::vector<uint8_t> rom = read_file(WAKECLK_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long calls = 200;
    Trip t;
    ASSERT_TRUE(round_trip(rom, Card{}, [] { return tb.api.size() >= 200; }, 0,
                           calls, true, false, t));
    long worst = 0;
    for (long i = 0; i < calls; i++)
    {
        long ab = (long)t.b.api[(size_t)i] - (long)t.a.api[(size_t)i];
        long ac = (long)t.c.api[(size_t)i] - (long)t.a.api[(size_t)i];
        worst = std::max(worst, std::max(std::labs(ab), std::labs(ac)));
    }
    fprintf(stderr, "  first call after the save: %u ms unsaved, %u saved, "
                    "%u woken\n",
            t.a.api[0], t.b.api[0], t.c.api[0]);
    ASSERT_LE(worst, 1L);
}

/* A save while the program has a file open for writing answers err, and
 * the program runs on to its end. */
UTEST(psleep, a_save_with_a_file_open_for_writing_answers_err)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom, Card{});
    const std::string name = "/Saves/rp6502/common/T.DAT";
    auto bound = [&] { return std::count(tb.bound, tb.bound + 16, name) > 0; };
    for (long i = 0; i < 20000000L && !bound(); i++)
        step();
    ASSERT_TRUE(bound());
    ASSERT_EQ(RES_ERR, save());
    for (long i = 0;
         i < 20000000L && tb.console.find("pocket file ok") == std::string::npos;
         i++)
        step();
    ASSERT_NE(std::string::npos, tb.console.find("pocket file ok"));
    ASSERT_EQ(0, tb.stray);
    teardown();
}

/* The characters of the terminal's cell memory, where the firmware prints:
 * a cell is a word, and a character of the code page is its low byte. */
static std::string screen()
{
    std::string s;
    for (size_t i = 0; i < 7200; i++)
        s += (char)MEM(mode0__DOT__cell0)[i];
    return s;
}

/* A hot reload of the ROM, which test_pocket.cpp's host_load describes as
 * measured on a Pocket: a slot request write, the image, its data table
 * entry and a second 0x008F. */
static void hot_reload(const std::vector<uint8_t> &rom)
{
    dut->dataslot_requestwrite_id = 0;
    dut->dataslot_requestwrite = 1;
    a_edge();
    a_edge();
    dut->dataslot_requestwrite = 0;
    dut->dataslot_allcomplete = 0;
    host_put_bytes(TB_STAGE_ROM_BASE, rom.data(), rom.size());
    dt_set(0, (uint32_t)rom.size());
    for (int i = 0; i < 40; i++)
        a_edge();
    dut->datatable_q = tb.dt[1];
    dut->dataslot_allcomplete = 1;
}

/* A savestate that fails its checks: the wake prints "bad savestate" and
 * the 6502 stays in reset, as it does after a program ends, until the user
 * picks a ROM, which starts. */
UTEST(psleep, a_bad_savestate_stops_the_machine_until_a_rom_is_picked)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot_held(rom, Card{});
    std::vector<uint8_t> blob(BLOB_BYTES, 0x5A);
    memcpy(blob.data(), "RP65", 4);
    put_blob(blob);
    ASSERT_EQ(ANSWER_LOAD_IN_RESET, sst_cmd(true, true, false));
    release_reset();
    for (long i = 0;
         i < 2000 && screen().find("bad savestate") == std::string::npos; i++)
        for (long k = 0; k < 10000; k++)
            step();
    ASSERT_NE(std::string::npos, screen().find("bad savestate"));
    for (long i = 0; i < 2000000L; i++)
        step();
    ASSERT_FALSE((int)MEM(resb));
    hot_reload(rom);
    for (long i = 0; i < 4000000L && !MEM(resb); i++)
        step();
    ASSERT_TRUE((int)MEM(resb));
    teardown();
}
