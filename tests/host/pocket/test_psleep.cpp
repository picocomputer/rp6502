/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "tb_snap.h"
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

enum { REQ_NONE = 0, REQ_OPEN, REQ_FLUSH, REQ_GETFILE, REQ_READ, REQ_WRITE };

/* The 6502's clock at a psg_tick: its PC and bus address, its PHI2
 * accumulator and VIA T1, so a load that resumes the 6502 a clock off shows
 * even where the mix does not. */
struct Cpu
{
    uint32_t pc, addr, acc, t1;
    bool operator!=(const Cpu &o) const
    {
        return pc != o.pc || addr != o.addr || acc != o.acc || t1 != o.t1;
    }
};

/* The bench's side of a session: everything the C++ host holds while the
 * machine runs. A fresh configuration starts from Bench{}, and a snapshot
 * keeps the model and a copy of this, which io() lists field by field. */
struct Bench
{
    long a_next = 0, s_next = 0, sys = 0;
    uint32_t dt_pipe[2] = {};
    uint32_t dt[64] = {};

    std::map<std::string, std::vector<uint8_t>> files;
    std::set<std::string> dirs;
    std::string bound[16];
    std::string console;
    std::string rv;
    int opens = 0, reads = 0, writes = 0, flushes = 0, getfiles = 0;
    /* Commands made of another session's registers: a Slot Write whose data
     * is not in the file window, the only place the firmware puts a write's
     * data, and a Slot Read that fills no file window or is of no bytes. */
    int wild = 0;
    /* Slot Reads and Slot Writes for a slot this host has bound to nothing.
     * The firmware asks what a slot holds before it uses one after a wake,
     * so one of these went out with the slot of the session that slept. */
    int stray = 0;
    /* Open File commands the core has raised. */
    int open_reqs = 0;

    int req = REQ_NONE, servicing = 0;
    int prev_r = 0, prev_w = 0, prev_o = 0, prev_f = 0, prev_g = 0;
    /* flush_hold leaves the next Flush unanswered, with flush_owed set,
     * until the test calls target_done(). */
    bool flush_hold = false, flush_owed = false;

    /* The mix at each rising psg_tick since boot, so a tick's index is its
     * count in machine time, and the 6502's clock at each of them.
     * stop_tick is the count when the machine's clock first stops after
     * stop_arm, so the first tick the restored machine takes is that
     * index. */
    std::vector<uint32_t> mix;
    std::vector<Cpu> cpu;
    int tick_prev = 0;
    bool stop_arm = false;
    long stop_tick = -1;
    /* The clk_sys count at which the soft CPU next halts. */
    bool halt_arm = false;
    long halt_sys = 0;
    /* How many reset sequences the 6502 has begun. A program that is
     * started, or started over, begins one, and a program that resumes does
     * not. */
    int resets = 0;
    int brk_res_prev = 0;
    /* The firmware's trap handler: how many times it has been entered, and
     * whether the soft CPU is between its entry and its mret. halt_trap is
     * that at the halt halt_arm waits for. */
    uint32_t trap_pc = 0;
    int traps = 0;
    bool in_trap = false, halt_trap = false;
    /* The byte of the blob at which a save first said err. */
    long underrun_at = 0;

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
        tb_snap_io(s, rv);
        tb_snap_io(s, opens);
        tb_snap_io(s, reads);
        tb_snap_io(s, writes);
        tb_snap_io(s, flushes);
        tb_snap_io(s, getfiles);
        tb_snap_io(s, wild);
        tb_snap_io(s, stray);
        tb_snap_io(s, open_reqs);
        tb_snap_io(s, req);
        tb_snap_io(s, servicing);
        tb_snap_io(s, prev_r);
        tb_snap_io(s, prev_w);
        tb_snap_io(s, prev_o);
        tb_snap_io(s, prev_f);
        tb_snap_io(s, prev_g);
        tb_snap_io(s, flush_hold);
        tb_snap_io(s, flush_owed);
        tb_snap_io(s, mix);
        tb_snap_io(s, cpu);
        tb_snap_io(s, tick_prev);
        tb_snap_io(s, stop_arm);
        tb_snap_io(s, stop_tick);
        tb_snap_io(s, halt_arm);
        tb_snap_io(s, halt_sys);
        tb_snap_io(s, resets);
        tb_snap_io(s, brk_res_prev);
        tb_snap_io(s, trap_pc);
        tb_snap_io(s, traps);
        tb_snap_io(s, in_trap);
        tb_snap_io(s, halt_trap);
        tb_snap_io(s, underrun_at);
    }
};
static Bench tb;

/* The firmware's trap handler: its entry, and the mret that ends it. */
static uint32_t g_trap_entry, g_trap_mret;

/* BLOB_BRIDGE and BLOB_BYTES are core_top's savestate_addr and
 * savestate_size. */
#define BLOB_BRIDGE 0x03E80000u
#define BLOB_BYTES 300368u

/* The blob's words up to its trailer, as the engine last summed them
 * before a save was ready, high byte first. The engine serves the host
 * these same words, so read_blob takes them from here. */
static std::vector<uint8_t> g_summed(BLOB_BYTES);

/* Called on every edge, a_edge's included, so no tick is missed. */
static void observe()
{
    auto *r = dut->rootp;
    int t = r->tb_pocket__DOT__core__DOT__machine__DOT__psg_tick;
    if (t && !tb.tick_prev)
    {
        tb.mix.push_back(
            (uint32_t)(uint16_t)r->tb_pocket__DOT__core__DOT__aud_l << 16
            | (uint16_t)r->tb_pocket__DOT__core__DOT__aud_r);
        tb.cpu.push_back(
            {(uint32_t)r->tb_pocket__DOT__core__DOT__machine__DOT__cpu__DOT__pc,
             (uint32_t)r->tb_pocket__DOT__core__DOT__machine__DOT__cpu_addr,
             (uint32_t)r
                 ->tb_pocket__DOT__core__DOT__machine__DOT__phi2__DOT__acc,
             (uint32_t)r
                 ->tb_pocket__DOT__core__DOT__machine__DOT__via__DOT__t1_counter});
    }
    tb.tick_prev = t;
    int br = r->tb_pocket__DOT__core__DOT__machine__DOT__cpu__DOT__brk_res;
    if (br && !tb.brk_res_prev)
        tb.resets++;
    tb.brk_res_prev = br;
    if (g_trap_entry)
    {
        uint32_t pc = (uint32_t)r
            ->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__cpu__DOT__core__DOT__decode_u__DOT__pc;
        if (pc == g_trap_entry && tb.trap_pc != pc)
        {
            tb.traps++;
            tb.in_trap = true;
        }
        else if (tb.trap_pc == g_trap_mret && pc != g_trap_mret)
            tb.in_trap = false;
        tb.trap_pc = pc;
    }
    if (tb.halt_arm
        && r->tb_pocket__DOT__core__DOT__machine__DOT__wiring_sst_dbg_halted)
    {
        tb.halt_arm = false;
        tb.halt_sys = tb.sys;
        tb.halt_trap = tb.in_trap;
    }
    if (tb.stop_arm && !r->tb_pocket__DOT__mach_clk_en)
    {
        tb.stop_arm = false;
        tb.stop_tick = (long)tb.mix.size();
    }
    if (r->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__summing
        && r->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__hold_valid)
    {
        uint32_t i = r->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__sum_idx;
        uint32_t w = r->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__hold;
        if (i == r->tb_pocket__DOT__core__DOT__machine__DOT__engine__DOT__hold_idx
            && i < BLOB_BYTES / 4)
        {
            g_summed[i * 4] = (uint8_t)(w >> 24);
            g_summed[i * 4 + 1] = (uint8_t)(w >> 16);
            g_summed[i * 4 + 2] = (uint8_t)(w >> 8);
            g_summed[i * 4 + 3] = (uint8_t)w;
        }
    }
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
        dut->datatable_q = tb.dt_pipe[1];
        tb.dt_pipe[1] = tb.dt_pipe[0];
        tb.dt_pipe[0] = tb.dt[dut->tb_pocket_dt_addr & 63];
        dut->clk_74a = 1;
    }
    dut->eval();
    if (sedge && dut->tb_pocket_tx_valid)
        tb.console += (char)dut->tb_pocket_tx_data;
    if (sedge && dut->tb_pocket_rv_tx_valid)
        tb.rv += (char)dut->tb_pocket_rv_tx_data;
    /* pocket_file raises each request line for a single clk_74a cycle,
     * and core_bridge_cmd queues a command on the rising edge of its
     * request line, so the lines are sampled on every tick() and not only
     * in step(). */
    {
        int r = dut->tb_pocket_ds_read, w = dut->tb_pocket_ds_write,
            o = dut->tb_pocket_ds_openfile, f = dut->tb_pocket_ds_flush,
            g = dut->tb_pocket_ds_getfile;
        if (o && !tb.prev_o)
        {
            tb.req = REQ_OPEN;
            tb.open_reqs++;
        }
        else if (f && !tb.prev_f)
            tb.req = REQ_FLUSH;
        else if (g && !tb.prev_g)
            tb.req = REQ_GETFILE;
        else if (r && !tb.prev_r)
            tb.req = REQ_READ;
        else if (w && !tb.prev_w)
            tb.req = REQ_WRITE;
        tb.prev_r = r;
        tb.prev_w = w;
        tb.prev_o = o;
        tb.prev_f = f;
        tb.prev_g = g;
    }
    if (sedge)
    {
        dut->clk_sys = 0;
        dut->clk_vid = 0;
        dut->clk_rv = 0;
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
    tb.dt[slot * 2] = slot;
    tb.dt[slot * 2 + 1] = size;
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
        tb.opens++;
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
    tb.opens++;
    bool created = false;
    auto it = tb.files.find(key);
    if (it == tb.files.end())
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
        if (!(flags & 2) || !tb.dirs.count(parent))
        {
            dut->target_dataslot_done = 1;
            dut->target_dataslot_err = 1;
            for (int k = 0; k < 4; k++)
                a_edge();
            return;
        }
        it = tb.files.emplace(key, std::vector<uint8_t>()).first;
        created = true;
    }
    if (flags & 2)
        it->second.resize(size, 0);
    tb.bound[slot] = key;
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
    if (slot < 16 && !tb.bound[slot].empty())
        return true;
    tb.stray++;
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
    tb.reads++;
    if (!len || (slot && (at < TB_STAGE_WIN_BASE
                          || at >= TB_STAGE_WIN_BASE + 8 * TB_STAGE_WIN_SIZE)))
        tb.wild++;
    if (!slot_bound(slot))
        return;
    std::vector<uint8_t> &f = tb.files[tb.bound[slot]];
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
    tb.writes++;
    if (at != 0x20000000u)
        tb.wild++;
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
    target_done();
}


static void do_flush()
{
    dut->target_dataslot_done = 0;
    tb.flushes++;
    /* pocket_file waits in F_ARM for target_dataslot_done to fall and
     * then in F_WAIT for it to rise, so done is held low across a few
     * clk_74a edges before target_done() raises it. */
    for (int k = 0; k < 4; k++)
        a_edge();
    if (tb.flush_hold)
    {
        tb.flush_hold = false;
        tb.flush_owed = true;
        return;
    }
    target_done();
}

static void do_getfile()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t at = dut->tb_pocket_resp_struct;
    tb.getfiles++;
    const std::string &name = tb.bound[slot];
    std::vector<uint8_t> resp(256, 0);
    for (size_t i = 0; i < name.size() && i + 1 < resp.size(); i++)
        resp[i] = (uint8_t)name[i];
    host_put_bytes(at, resp.data(), resp.size());
    target_done();
}

static void step()
{
    tick();
    if (!tb.req || tb.servicing)
        return;
    int req = tb.req;
    tb.req = REQ_NONE;
    tb.servicing = 1;
    switch (req)
    {
    case REQ_OPEN: do_openfile(); break;
    case REQ_FLUSH: do_flush(); break;
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


/* The core has until the next read strobe to drive bridge_rd_data,
 * because io_bridge_peripheral.v buffers reads by one word. HOST_GAP is
 * about one microsecond of clk_74a, which is shorter than the 88 cycles
 * that io_bridge_peripheral.v gives as the fastest host access. */
#define HOST_GAP 74


/* A symbol of the firmware that is loaded, from the ELF beside its image.
 * Returns false when the ELF has no such symbol. */
static std::vector<uint8_t> read_file(const char *path);

static bool fw_symbol(const char *name, uint32_t *addr, uint32_t *size)
{
    std::string path = SW_BIN;
    path.replace(path.rfind(".bin"), 4, ".elf");
    std::vector<uint8_t> e = read_file(path.c_str());
    auto u16 = [&](size_t at) { return (uint32_t)e[at] | (uint32_t)e[at + 1] << 8; };
    auto u32 = [&](size_t at) { return u16(at) | u16(at + 2) << 16; };
    if (e.size() < 52 || memcmp(e.data(), "\x7f" "ELF\x01\x01", 6))
        return false;
    size_t shoff = u32(32), shentsize = u16(46), shnum = u16(48);
    for (size_t i = 0; i < shnum; i++)
    {
        size_t sh = shoff + i * shentsize;
        if (sh + 40 > e.size() || u32(sh + 4) != 2 /* SHT_SYMTAB */)
            continue;
        size_t off = u32(sh + 16), n = u32(sh + 20) / 16;
        size_t str = u32(shoff + u32(sh + 24) * shentsize + 16);
        for (size_t k = 0; k < n; k++)
        {
            size_t sym = off + k * 16;
            /* Link-time optimisation adds a suffix after a dot. */
            const char *have = (const char *)e.data() + str + u32(sym);
            size_t len = strlen(name);
            if (!strncmp(have, name, len) && (!have[len] || have[len] == '.'))
            {
                *addr = u32(sym + 4);
                *size = u32(sym + 8);
                return true;
            }
        }
    }
    return false;
}

/* The firmware every session of a test is loaded with. Nothing in this
 * firmware makes a misaligned access today: the compiler writes the errno
 * at $FFED, the one odd halfword, as two bytes. g_trap_fw turns the first
 * of those two stores into a halfword store, so every failed API call takes
 * the firmware through its trap handler, which is the firmware's own. The
 * second store then writes the high byte again. */
static bool g_trap_fw;

static std::vector<uint8_t> fw_image()
{
    std::vector<uint8_t> fw = tb_stage_firmware(SW_BIN);
    uint32_t at, size;
    if (!g_trap_fw || !fw_symbol("api_return_errno", &at, &size))
        return fw;
    for (uint32_t end = at + size; at + 4 <= end; at += 2)
    {
        uint32_t insn = (uint32_t)fw[at] | (uint32_t)fw[at + 1] << 8
                        | (uint32_t)fw[at + 2] << 16
                        | (uint32_t)fw[at + 3] << 24;
        /* sb rs2, 13(rs1) */
        if ((insn & 0xFE00707Fu) == 0x00000023u && ((insn >> 7) & 31) == 13)
        {
            fw[at + 1] |= 0x10;
            return fw;
        }
    }
    return fw;
}

/* The firmware's build stamp is the word at this offset of its image. */
#define FW_STAMP 4u

/* The bench of a fresh configuration, on the card as it is. */
static void bench_fresh()
{
    Bench fresh;
    fresh.files = std::move(tb.files);
    fresh.dirs = std::move(tb.dirs);
    tb = std::move(fresh);
}

/* The card of a fresh configuration: the folders and no files. */
static void card_fresh()
{
    tb.files.clear();
    tb.dirs = {"/", "/Assets", "/Assets/rp6502", "/Assets/rp6502/common",
               "/Saves", "/Saves/rp6502", "/Saves/rp6502/common"};
}

/* A fresh configuration with rom loaded, on the card as it is or on a
 * fresh one, just out of reset. */
static void boot_into(const std::vector<uint8_t> &rom, bool keep_card)
{
    dut = new Vtb_pocket;
    bench_fresh();
    if (!keep_card)
        card_fresh();
    tb.bound[0] = "/Assets/rp6502/common/file.rp6502";
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
                  TB_STAGE_MEM_BASE, fw_image());

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

    dut->datatable_q = tb.dt[1];
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

/* Set to boot every time and write the snapshot again, and whether the last
 * boot_to started from a snapshot. */
static bool g_snap_fresh, g_snap_restored;

/* Starts a fresh configuration with rom where run() leaves it. The first
 * time, it boots with boot_into and runs run(), and if run() returns true
 * it keeps a snapshot, which every later boot_to of the same label and the
 * same inputs starts from. The inputs are the ROM, the firmware, the card
 * when keep_card is set, and the trap handler that observe() watches, so
 * run() must depend on nothing else. Returns what run() returned, or true
 * from a snapshot. */
template <typename Run>
static bool boot_to(const std::string &label, const std::vector<uint8_t> &rom,
                    bool keep_card, Run run)
{
    tb_snap_key k;
    tb_snap_io(k, label);
    tb_snap_io(k, rom);
    tb_snap_io(k, fw_image());
    tb_snap_io(k, keep_card);
    if (keep_card)
    {
        tb_snap_io(k, tb.files);
        tb_snap_io(k, tb.dirs);
    }
    tb_snap_io(k, g_trap_entry);
    tb_snap_io(k, g_trap_mret);
    k.flush();
    std::string path = tb_snap_path(tb_snap_dir(TEST_SCRATCH, "psleep"), k.h);
    std::vector<uint8_t> v;
    g_snap_restored = !g_snap_fresh && tb_snap_read(path, v);
    if (g_snap_restored)
    {
        dut = tb_snap_make<Vtb_pocket>(v, tb);
        return true;
    }
    boot_into(rom, keep_card);
    if (!run())
        return false;
    tb_snap_write(path, tb_snap_take(*dut, tb));
    return true;
}

/* The result codes of the host's two savestate commands. */
enum { RES_IDLE = 0, RES_BUSY = 1, RES_OK = 2, RES_ERR = 3 };

/* What the core answers at a savestate request, which this model of the
 * Pocket takes as the request going well. These are guesses. Analogue
 * documents the four codes and not which of them a Pocket goes on from,
 * and no Pocket has been watched taking these. Every test that asks for a
 * save or a load holds the core to them, here and nowhere else, so what
 * hardware shows is one line to change. */
/* A save, or a load into the running machine: busy, then ok on a query. */
static const int ANSWER_SAVE = RES_BUSY;
static const int ANSWER_LOAD_RUNNING = RES_BUSY;
/* A load while the Pocket holds the core in reset: ok at once. */
static const int ANSWER_LOAD_IN_RESET = RES_OK;
/* A save while a file is open for writing: err at once. What the Pocket
 * then does with a sleep is not known either. */
static const int ANSWER_SAVE_REFUSED = RES_ERR;

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

/* Steps until the save that was asked for has its blob ready to read. */
static bool save_wait(void)
{
    for (long i = 0; i < 80000000L && dut->tb_pocket_savestate_start_busy; i++)
    {
        step();
    }
    return sst_cmd(false, false, true) == RES_OK;
}

/* Starts a save and steps until the blob is ready to read. */
static bool save_ready(void)
{
    return sst_cmd(false, true, true) == ANSWER_SAVE && save_wait();
}

/* The host reads the blob's words from byte from on over the bridge, a
 * word at a time. */
static void read_blob_from(std::vector<uint8_t> &blob, uint32_t from)
{
    for (uint32_t i = from; i < BLOB_BYTES; i += 4)
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
        if (dut->tb_pocket_savestate_start_err && !tb.underrun_at)
            tb.underrun_at = (long)i;
    }
}

/* The whole blob over the bridge, as the host reads it. */
static void read_blob_bridge(std::vector<uint8_t> &blob)
{
    blob.assign(BLOB_BYTES, 0);
    read_blob_from(blob, 0);
}

/* The blob with no bridge traffic but its trailer: the words before the
 * trailer as the engine summed them, and the trailer's four words over the
 * bridge, whose last ends the save as the host's last read does.
 * psleep.a_running_program_survives_the_reconfigure holds this to
 * read_blob_bridge. */
static void read_blob(std::vector<uint8_t> &blob)
{
    blob = g_summed;
    read_blob_from(blob, BLOB_BYTES - 16);
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
static void put_blob_bridge(const std::vector<uint8_t> &file)
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

/* The file with no bridge traffic but its first word: the whole file
 * straight into the SDRAM where the host writes it, and then its first
 * word over the bridge, which is how the core learns a blob has come. */
static void put_blob(const std::vector<uint8_t> &file)
{
    tb_stage_poke(dut->rootp->tb_pocket__DOT__chip__DOT__mem, BLOB_BRIDGE,
                  file);
    host_write(BLOB_BRIDGE, (uint32_t)file[0] << 24 | (uint32_t)file[1] << 16
                                | (uint32_t)file[2] << 8 | file[3]);
}

/* A load into the running machine: busy until the engine has finished,
 * then ok or err. */
static bool load_state(void)
{
    if (sst_cmd(true, true, true) != ANSWER_LOAD_RUNNING)
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

static void boot_to_save_point(const std::vector<uint8_t> &rom, bool keep_card)
{
    boot_to("save point", rom, keep_card, [] {
        step_to_save_point();
        return true;
    });
}

/* The stream program with payload on the card as M.DAT, run until it has
 * printed 64 bytes of it. */
static void boot_to_streaming(const std::vector<uint8_t> &rom,
                              const std::vector<uint8_t> &payload)
{
    card_fresh();
    tb.files["/Assets/rp6502/common/M.DAT"] = payload;
    boot_to("streaming", rom, true, [] {
        for (long i = 0; i < 20000000L && tb.console.size() < 64; i++)
            step();
        return true;
    });
}

/* The PCM program, run until its stream is open and holds samples. */
static bool boot_to_pcm(const std::vector<uint8_t> &rom)
{
    return boot_to("pcm playing", rom, false, [] {
        long guard = 0;
        while (!(MEM(pcm__DOT__open) && MEM(pcm_level)) && guard++ < 20000000L)
            step();
        return guard < 20000000L;
    });
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
    ASSERT_EQ(0u, tb.console.size());
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

    /* This case moves the blob over the bridge both ways, and holds what
     * every other case takes without the bridge to what the host reads. */
    std::vector<uint8_t> blob;
    ASSERT_TRUE(save_ready());
    read_blob_bridge(blob);
    ASSERT_TRUE(std::equal(blob.begin(), blob.end() - 16, g_summed.begin()));
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
    if (tb.underrun_at)
        fprintf(stderr, "underrun at byte %ld, word %ld\n",
                tb.underrun_at, tb.underrun_at / 4);
    ASSERT_FALSE((int)dut->tb_pocket_savestate_start_err);

    for (long i = 0; i < 20000000L && tb.console.find(DONE) == std::string::npos;
         i++)
    {
        step();
        if ((i % 4000000L) == 3999999L)
            fprintf(stderr, "after save t=%ldM pc=%04x resb=%d eng=%d "
                            "con=%zu rv=%zu\n",
                    i / 1000000L, (unsigned)MEM(cpu__DOT__pc), (int)MEM(resb),
                    (int)MEM(engine__DOT__state), tb.console.size(),
                    tb.rv.size());
    }
    ASSERT_TRUE(tb.console.find(DONE) != std::string::npos);
    ASSERT_EQ(align_pre, vid_offset(3L * 525 * 1600));

    teardown();

    boot_to_save_point(rom, false);
    ASSERT_NE(0xA0u, (uint32_t)MEM(xram__DOT__mem0)[0]);
    ASSERT_TRUE((int)MEM(resb));
    ASSERT_TRUE(tb.console.find(DONE) == std::string::npos);

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    put_blob_bridge(file);

    ASSERT_TRUE(load_state());
    /* The SDRAM holds the file as put_blob puts it there. */
    for (size_t i = 0; i < file.size(); i += 2)
        ASSERT_EQ((uint32_t)(file[i] | file[i + 1] << 8),
                  (uint32_t)dut->rootp->tb_pocket__DOT__chip__DOT__mem
                      [(BLOB_BRIDGE + i) >> 1]);
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

    for (long i = 0; i < 20000000L && tb.console.find(DONE) == std::string::npos;
         i++)
    {
        step();
        if ((i % 2000000L) == 1999999L)
            fprintf(stderr, "t=%ldM pc=%04x resb=%d con=%zu rv=%zu\n",
                    i / 1000000L, (unsigned)MEM(cpu__DOT__pc),
                    (int)MEM(resb), tb.console.size(), tb.rv.size());
    }
    if (tb.console.find(DONE) == std::string::npos)
        fprintf(stderr,
                "6502 pc %04x resb=%d running=%d engine=%d console=[%s] rv=[%s]\n",
                (unsigned)MEM(cpu__DOT__pc), (int)MEM(resb),
                (int)dut->rootp->tb_pocket__DOT__mach_clk_en, (int)MEM(engine__DOT__state),
                tb.console.c_str(), tb.rv.c_str());
    ASSERT_TRUE(tb.console.find(DONE) != std::string::npos);
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

    boot_to_streaming(rom, payload);
    ASSERT_GE(tb.console.size(), 64u);
    ASSERT_LT(tb.console.size(), want.size());
    std::string before = tb.console;

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot_to_save_point(other, true);

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    put_blob(file);
    for (long i = 0; i < 2000000L && (int)MEM(resb); i++)
        step();
    ASSERT_FALSE((int)MEM(resb));
    ASSERT_TRUE(load_state());
    tb.console.clear();

    for (long i = 0; i < 40000000L
                     && tb.console.find("stream ok\r\n") == std::string::npos;
         i++)
        step();
    ASSERT_TRUE(tb.console.find("stream ok\r\n") != std::string::npos);
    std::string want_tail = want.substr(before.size()) + "stream ok\r\n";
    if (want_tail != tb.console)
    {
        size_t at = 0;
        while (at < want_tail.size() && at < tb.console.size()
               && want_tail[at] == tb.console[at])
            at++;
        size_t from = at > 8 ? at - 8 : 0;
        fprintf(stderr,
                "join: before=%zu want=%zu got=%zu differ at %zu\n",
                before.size(), want_tail.size(), tb.console.size(), at);
        for (size_t i = from; i < at + 8; i++)
            fprintf(stderr, "  %zu want=%02x got=%02x\n", i,
                    i < want_tail.size() ? (unsigned)(uint8_t)want_tail[i] : 0,
                    i < tb.console.size() ? (unsigned)(uint8_t)tb.console[i] : 0);
    }
    ASSERT_TRUE(want_tail == tb.console);
    teardown();
}

UTEST(psleep, a_load_into_a_running_machine_keeps_its_bindings)
{
    std::vector<uint8_t> rom = read_file(STREAM_ROM);
    ASSERT_GT(rom.size(), 0u);
    std::vector<uint8_t> payload;
    for (int i = 0; i < 512; i++)
        payload.push_back((uint8_t)('a' + (i % 26)));

    boot_to_streaming(rom, payload);
    ASSERT_GE(tb.console.size(), 64u);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    put_blob(file);
    int opens_before = tb.opens, getfiles_before = tb.getfiles;
    ASSERT_TRUE(load_state());
    tb.console.clear();

    for (long i = 0; i < 40000000L
                     && tb.console.find("stream ok\r\n") == std::string::npos;
         i++)
        step();
    ASSERT_TRUE(tb.console.find("stream ok\r\n") != std::string::npos);
    ASSERT_GT(tb.getfiles, getfiles_before);
    ASSERT_EQ(opens_before, tb.opens);
    teardown();
}

/* The save comes as the host answers the program's read, with the file
 * open only for reading. */
UTEST(psleep, a_sleep_inside_a_file_operation_still_finishes_it)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);

    for (long i = 0; i < 20000000L && tb.reads == 0; i++)
        step();
    ASSERT_GT(tb.reads, 0);
    ASSERT_TRUE(tb.console.find(DONE) == std::string::npos);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    boot_to_save_point(rom, true);
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    put_blob(file);
    ASSERT_TRUE(load_state());
    tb.console.clear();

    for (long i = 0; i < 40000000L && tb.console.find(DONE) == std::string::npos;
         i++)
        step();
    ASSERT_TRUE(tb.console.find(DONE) != std::string::npos);
    teardown();
}

/* The main loop stops the program when a savestate starts to load, so the
 * program stops while its close is still waiting for the Flush reply only if
 * the close polls Flush instead of blocking the main loop. */
UTEST(psleep, a_close_waiting_on_flush_leaves_the_main_loop_running)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    tb.flush_hold = true;

    for (long i = 0; i < 20000000L && !tb.flush_owed; i++)
        step();
    ASSERT_TRUE(tb.flush_owed);
    ASSERT_TRUE((int)MEM(resb));

    host_write(BLOB_BRIDGE, 0);
    for (long i = 0; i < 2000000L && (int)MEM(resb); i++)
        step();
    ASSERT_FALSE((int)MEM(resb));

    target_done();
    tb.flush_owed = false;
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
    boot_to_save_point(other, false);
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
    boot_to_save_point(other, false);
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
    ASSERT_TRUE(boot_to_pcm(rom));
    long guard;
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
    boot_to_save_point(other, false);
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
    ASSERT_TRUE(boot_to_pcm(rom));
    long guard;
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
        boot_to_save_point(other, false);
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
 * 1050 clocks, so a tick is about 2,600 steps. The window is 1,600 ticks,
 * 33 ms. The loop refills nothing, so PCM, about 2,600 bytes full at the
 * save, runs dry about 1,430 ticks in, and the OPL takes a burst every 200
 * ticks. */
#define WINDOW 1600L

static void run_to(char c, long settle)
{
    for (long i = 0; i < 80000000L && tb.console.find(c) == std::string::npos;
         i++)
        step();
    for (long i = 0; i < settle; i++)
        step();
}

/* A fresh configuration with rom, run until the console shows c. */
static void boot_to_console(const std::vector<uint8_t> &rom, char c)
{
    boot_to(std::string("console ") + c, rom, false, [&] {
        run_to(c, 0);
        return true;
    });
}

static void run_ticks(size_t n)
{
    for (long i = 0; i < 200000000L && tb.mix.size() < n; i++)
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
    boot_to_save_point(other, false);
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    put_blob(file);
    tb.stop_arm = true;
    if (!load_state())
        return false;
    c.at = tb.stop_tick;
    run_ticks((size_t)(c.at + n));
    c.end = tail();
    c.mix = tb.mix;
    c.cpu = tb.cpu;
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
        boot_to_console(rom, 'W');
        run_to('W', 620000L + 1235L * k);
        tb.stop_arm = true;
        ASSERT_TRUE(save_ready());
        at[k] = tb.stop_tick;
        fprintf(stderr, "  save %d at tick %ld: level %u, queue %u\n", k,
                at[k], (unsigned)MEM(pcm_level),
                (unsigned)MEM(opl__DOT__q_len));
        ASSERT_GT((uint32_t)MEM(pcm_level), 1024u);
        read_blob(blob[k]);
        run_ticks((size_t)(at[k] + WINDOW));
        b_end[k] = tail();
        b_mix[k] = tb.mix;
        b_cpu[k] = tb.cpu;
        teardown();
    }

    boot_to_console(rom, 'W');
    Tail a_end[2];
    for (int k = 0; k < 2; k++)
    {
        run_ticks((size_t)(at[k] + WINDOW));
        a_end[k] = tail();
    }
    std::vector<uint32_t> a_mix = tb.mix;
    std::vector<Cpu> a_cpu = tb.cpu;
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

/* Probes for a save whose stop finds the machine as hit() wants it, a
 * probe every 4,001 steps and a few more, counting probes in *probes. A
 * probe forks the session and asks for a save. The engine's sum waits for
 * the machine to stop, so once the sum is past its first word, hit() reads
 * the stopped machine: -1 for a miss, or what the caller wants back. A
 * miss goes back to the fork, so the session runs on as if no save had
 * been asked for. A hit waits until the blob is ready, and hit() must find
 * the machine the same then. Returns the hit, -1 when every probe up to
 * max missed, or -2 for a save that went wrong. */
template <typename Hit> static int probe_save(int *probes, int max, Hit hit)
{
    /* The fork keeps its capacity from one probe to the next, and a miss
     * loads it back into the same model. */
    static std::vector<uint8_t> fork;
    for (; *probes < max; ++*probes)
    {
        for (long i = 0; i < 4001 + 37L * (*probes % 29); i++)
            step();
        tb_snap_take(*dut, tb, fork);
        tb.stop_arm = true;
        if (sst_cmd(false, true, true) != ANSWER_SAVE)
            return -2;
        for (long i = 0; i < 20000000L && !MEM(engine__DOT__sum_idx); i++)
            step();
        int h = hit();
        if (h >= 0)
        {
            ++*probes;
            return save_wait() && hit() == h ? h : -2;
        }
        tb_snap_load(*dut, fork, tb);
    }
    return -1;
}

/* Saves made while the 6502 floods RW0 and RW1, until one stops with the
 * snooped write in flight and one with the RW engine's write pending, both
 * bound for the OPL, whose queue is full, so a lost write shows in the
 * drop count. Each loads into another session and matches the session
 * that never saved in the mix, the 6502's clock and the count. */
UTEST(psleep, a_snoop_in_flight_comes_back)
{
    std::vector<uint8_t> rom = read_file(AUD_SNOOP_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long n = 1500;
    std::vector<uint8_t> blob[2];
    long at[2] = {-1, -1};
    boot_to_console(rom, 'W');
    run_to('W', 200000L);
    int probes = 0;
    while (at[0] < 0 || at[1] < 0)
    {
        int k = probe_save(&probes, 400, [&] {
            bool qs = MEM(qs_we) && (MEM(qs_addr) >> 8) == 0xF0;
            bool rw = MEM(ria__DOT__xr_wr_pend)
                      && (MEM(ria__DOT__xr_wr_addr) >> 8) == 0xF0;
            return qs && at[0] < 0 ? 0 : rw && at[1] < 0 ? 1 : -1;
        });
        if (k < 0)
            break;
        at[k] = tb.stop_tick;
        read_blob(blob[k]);
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
    }
    fprintf(stderr, "  %d probes: the snoop at tick %ld, the RW write at %ld\n",
            probes, at[0], at[1]);
    ASSERT_GE(at[0], 0L);
    ASSERT_GE(at[1], 0L);
    run_ticks((size_t)(std::max(at[0], at[1]) + n));
    std::vector<uint32_t> b_mix = tb.mix;
    std::vector<Cpu> b_cpu = tb.cpu;
    teardown();

    boot_to_console(rom, 'W');
    int first = at[0] < at[1] ? 0 : 1;
    Tail a_end[2];
    run_ticks((size_t)(at[first] + n));
    a_end[first] = tail();
    run_ticks((size_t)(at[1 - first] + n));
    a_end[1 - first] = tail();
    std::vector<uint32_t> a_mix = tb.mix;
    std::vector<Cpu> a_cpu = tb.cpu;
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
    boot_to_console(rom, 'W');
    run_to('W', 620000L);
    int probes = 0;
    ASSERT_EQ(0, probe_save(&probes, 200, [] { return MEM(phi2_en) ? 0 : -1; }));
    at = tb.stop_tick;
    read_blob(blob);
    fprintf(stderr, "  %d probes\n", probes);
    ASSERT_GE(at, 0L);
    run_ticks((size_t)(at + n));
    std::vector<uint32_t> b_mix = tb.mix;
    std::vector<Cpu> b_cpu = tb.cpu;
    teardown();

    boot_to_console(rom, 'W');
    run_ticks((size_t)(at + n));
    std::vector<uint32_t> a_mix = tb.mix;
    std::vector<Cpu> a_cpu = tb.cpu;
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
    boot_to_console(rom, 'W');
    run_to('W', 620000L);
    int probes = 0;
    ASSERT_EQ(0, probe_save(&probes, 200, [] {
                  bool released = MEM(psg__DOT__ch_adsr)[0] == 0
                                  && !MEM(qs_we) && !MEM(ria__DOT__xr_wr_pend);
                  return released && MEM(xr_busy) && MEM(ria__DOT__xr_wr_byte)
                             ? 0 : -1;
              }));
    read_blob(blob);
    fprintf(stderr, "  %d probes\n", probes);
    ASSERT_EQ((uint32_t)MEM(ria__DOT__xr_wr_byte),
              blob_get(blob, B_REGS + REGS_XR) >> 16 & 0xFF);
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
        boot_to_save_point(other, false);
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
 * phase, and the snooped write in flight. The window is 600 ticks. The
 * LFO's change shows last, about 195 ticks in. The snooped write's shows
 * in the drop count, as the OPL takes the write on its first edge after
 * the load. */
UTEST(psleep, a_corrupt_audio_word_diverges)
{
    std::vector<uint8_t> rom = read_file(AUD_SST_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long n = 600;
    boot_to_console(rom, 'W');
    run_to('W', 620000L);
    tb.stop_arm = true;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(save_ready());
    long at = tb.stop_tick;
    read_blob(blob);
    run_ticks((size_t)(at + n));
    std::vector<uint32_t> b_mix = tb.mix;
    teardown();

    std::vector<uint8_t> rom2 = read_file(AUD_SNOOP_ROM);
    boot_to_console(rom2, 'W');
    run_to('W', 200000L);
    std::vector<uint8_t> snoop;
    long sat = -1;
    int probes = 0;
    ASSERT_EQ(0, probe_save(&probes, 400, [] {
                  return MEM(qs_we) && (MEM(qs_addr) >> 8) == 0xF0 ? 0 : -1;
              }));
    sat = tb.stop_tick;
    read_blob(snoop);
    fprintf(stderr, "  %d probes\n", probes);
    long guard = 0;
    while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
        step();
    ASSERT_GE(sat, 0L);
    run_ticks((size_t)(sat + n));
    std::vector<uint32_t> s_mix = tb.mix;
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
        for (long i = 0; i < 200000000L && (long)tb.mix.size() < limit; i++)
        {
            step();
            int g = gate();
            if (prev && !g && t < 0)
                t = (long)tb.mix.size();
            prev = g;
        }
        return t;
    };

    boot_to_console(rom, '\a');
    run_to('\a', 0);
    for (long i = 0; i < 80000000L && !gate(); i++)
        step();
    ASSERT_TRUE(gate());
    for (long i = 0; i < 370000L; i++)
        step();
    ASSERT_TRUE(gate());
    tb.stop_arm = true;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    long at = tb.stop_tick;
    teardown();

    boot_to_console(rom, '\a');
    long a_rel = release_tick(at + 1600);
    std::vector<uint32_t> a_mix = tb.mix;
    teardown();
    ASSERT_GT(a_rel, at);

    std::vector<uint8_t> other = read_file(OPL_BEL_ROM);
    boot_to_save_point(other, false);
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    put_blob(file);
    tb.stop_arm = true;
    ASSERT_TRUE(load_state());
    long c_at = tb.stop_tick;
    ASSERT_TRUE(gate());
    long c_rel = release_tick(c_at + 1600);
    std::vector<uint32_t> c_mix = tb.mix;
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

/* A little-endian word of the 6502's RAM. */
static uint16_t ram16(uint16_t a)
{
    auto &m = dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__sram__DOT__mem;
    return (uint16_t)(m[a] | m[a + 1] << 8);
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
    boot_to_save_point(rom, false);
    tb.stop_arm = true;
    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    long at = tb.stop_tick;
    run_ticks((size_t)at + n);
    long b_clk = ram16(0x10);
    teardown();

    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot_to_save_point(other, false);
    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    put_blob(file);
    tb.stop_arm = true;
    ASSERT_TRUE(load_state());
    long c_at = tb.stop_tick;
    run_ticks((size_t)c_at + n);
    long c_clk = ram16(0x10);
    teardown();
    fprintf(stderr, "  the clock at tick %zu: %ld ms in B, %ld in C\n", n,
            b_clk, c_clk);
    ASSERT_GT(b_clk, 0L);
    ASSERT_LE(c_clk - b_clk < 0 ? b_clk - c_clk : c_clk - b_clk, 1L);
}

/* A machine started from a snapshot is the machine that booted. One boots
 * to the save point and writes the snapshot, one starts from it, both run
 * on the same steps, and they end in the same state, the model's and the
 * bench's alike. The steps take the file program through its writes and
 * reads, so the host's card and counts move as well. */
UTEST(psleep, a_restored_machine_is_the_booted_one)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    const long n = 3000000L;
    std::vector<uint8_t> state[2];
    for (int k = 0; k < 2; k++)
    {
        g_snap_fresh = k == 0;
        boot_to_save_point(rom, false);
        g_snap_fresh = false;
        ASSERT_EQ(k == 1, g_snap_restored);
        for (long i = 0; i < n; i++)
            step();
        state[k] = tb_snap_take(*dut, tb);
        teardown();
    }
    ASSERT_NE(std::string::npos, tb.console.find(DONE));
    ASSERT_EQ(state[0].size(), state[1].size());
    ASSERT_TRUE(state[0] == state[1]);
}

/* ------------------------------------------------------------------ */
/* Sleep and wake round trips with what the tests above hold fixed made
 * random: when the save comes, how the host reads the blob, and on the wake
 * the order and pace of the slot writes, Reset Exit, the blob and the load
 * command. PWAKE_SEED is the first seed and PWAKE_N the number of trips;
 * every other PWAKE_ variable pins one choice, to minimise a failure. The
 * host's pace and order with the blob are used only with PWAKE_BRIDGE set,
 * which moves the blob over the bridge; a trip without it takes the blob
 * with read_blob and gives it with put_blob.
 *
 * A trip holds the wake to what the session that slept went on to do. A
 * save the core refuses, with a file open for writing, is a trip too: the
 * session runs on as if nothing had been asked. */

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
    long until = tb.a_next + (long)gapn * 224;
    while (tb.a_next < until)
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
    int prog;         /* 0 file, 1 aud_snoop, 2 mode3, 3 trap */
    long save_at;     /* steps after the program's mark */
    int rmode, rbase, rorder;
    int refuse;       /* 0 good blob, 1 other build */
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
    int bridge;       /* the blob over the bridge both ways, at the pace
                       * and in the order above; else read_blob and
                       * put_blob, and those choices go unused */
};

/* A wake's Reset Exit against its blob and its load command. Analogue
 * documents no order, and cores report the load arriving in reset. */
enum
{
    /* Reset Exit, then the load. drb puts the blob before Reset Exit or
     * so long after it, and dbl puts the load after the blob. */
    SEQ_EXIT_FIRST = 0,
    /* The blob and the load in reset, by a Pocket that waits: it polls
     * the load's status until that reads ok and only then sends Reset
     * Exit. A core that answered busy until its engine had run would keep
     * this one in reset for good. */
    SEQ_LOAD_WAIT_EXIT,
    /* The blob and the load in reset, by a Pocket that does not wait: it
     * sends Reset Exit as soon as the request is acknowledged and never
     * reads the answer. Were the answer busy until the engine had run, it
     * would still be busy at this Reset Exit. */
    SEQ_LOAD_EXIT,
    /* The load in reset, then the blob, then Reset Exit. */
    SEQ_LOAD_BLOB_EXIT
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
    uint32_t seq = r.below(7);
    t.seq = (int)env_long("PWAKE_SEQ", seq < 4 ? SEQ_EXIT_FIRST
                                               : (long)seq - 3);
    t.bridge = (int)env_long("PWAKE_BRIDGE", 0);
}

static void pw_print(const Trip &t, const char *what)
{
    fprintf(stderr,
            "PWAKE %s seed=%ld prog=%d save_at=%ld rmode=%d rbase=%d "
            "rorder=%d refuse=%d fwbridge=%d fwfirst=%d drb=%ld wmode=%d "
            "wbase=%d wshuffle=%d service=%d dbl=%ld headpad=%d seq=%d "
            "bridge=%d\n",
            what, t.seed, t.prog, t.save_at, t.rmode, t.rbase, t.rorder,
            t.refuse, t.fw_bridge, t.fw_first, t.drb, t.wmode, t.wbase,
            t.wshuffle, t.service, t.dbl, t.head_pad, t.seq, t.bridge);
}

static std::vector<uint8_t> pw_rom(int prog)
{
    switch (prog)
    {
    case 0: return read_file(FILE_ROM);
    case 1: return read_file(AUD_SNOOP_ROM);
    case 2: return read_file(ROMS_DIR "/mode3_1bpp.rp6502");
    default: return read_file(SLEEPTRAP_ROM);
    }
}

/* What a program that ends prints last, or nothing for one that is judged
 * by its sound or its picture. */
static const char *pw_last_words(int prog)
{
    switch (prog)
    {
    case 0: return "pocket file ok";
    case 3: return "trap ok";
    default: return nullptr;
    }
}

/* Steps to the program's mark: the 6502 out of reset, the W the audio and
 * the trap programs print, or the picture program's canvas. */
static bool pw_to_mark(int prog)
{
    long guard = 0;
    if (prog == 0)
    {
        while (!(int)MEM(resb) && guard++ < 20000000L)
            step();
        return guard < 20000000L;
    }
    if (prog == 2)
    {
        while (!(uint32_t)MEM(prog__DOT__canvas_shadow)
               && guard++ < 20000000L)
            step();
        return guard < 20000000L;
    }
    while (tb.console.find('W') == std::string::npos && guard++ < 80000000L)
        step();
    return guard < 80000000L;
}

/* A fresh configuration with the program, on the card as it is, run to the
 * program's mark. */
static bool boot_to_mark(const std::vector<uint8_t> &rom, int prog)
{
    return boot_to("mark " + std::to_string(prog), rom, true,
                   [&] { return pw_to_mark(prog); });
}

/* Finds the firmware's trap handler for observe(): its entry, and the mret
 * that ends it, the first one in the image after the entry. */

static bool pw_find_trap()
{
    uint32_t size;
    if (!fw_symbol("_trap_entry", &g_trap_entry, &size))
        return false;
    std::vector<uint8_t> fw = tb_stage_firmware(SW_BIN);
    for (uint32_t at = g_trap_entry; at + 4 <= fw.size(); at += 2)
        if (fw[at] == 0x73 && fw[at + 1] == 0x00 && fw[at + 2] == 0x20
            && fw[at + 3] == 0x30)
        {
            g_trap_mret = at;
            return true;
        }
    return false;
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
    dut->datatable_q = tb.dt[1];
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
}

/* Steps until the load is over: the engine idle and the firmware done
 * with it. A load asked for in reset answers ok from its request on, and
 * one asked of the running machine answers busy and then ok or err. The
 * host answers no file request before the load's answer when quiet is
 * set, and holds the core to no answer when heeds is clear. Returns false
 * when the load never ends or a status line is wrong. */
static bool pw_load_over(bool early, bool quiet, bool heeds)
{
    bool fine = true;
    long i = 0;
    for (; i < 60000000L
           && (SST(load_act) || (int)MEM(engine__DOT__state) != 0);
         i++)
    {
        bool busy = dut->tb_pocket_savestate_load_busy;
        if (heeds && early && (busy || !dut->tb_pocket_savestate_load_ok))
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
    if ((heeds && res != (early ? ANSWER_LOAD_IN_RESET
                                : refused ? RES_ERR : RES_OK))
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

/* What pw_wake saw as it asked for the load: how many reset sequences the
 * 6502 had begun, and whether the card was still as the sleep left it. */
static int g_resets_at_load;
static bool g_card_kept;

/* A wake as the Pocket makes it: a fresh core in reset, the data slots
 * written, and then Reset Exit, the blob and the load command in the
 * trip's order. Returns 0 for a blob loaded, 1 for one refused, and 2 for
 * a load that never ended or a wrong answer. */
static int pw_wake(Rng &r, const Trip &t, const std::vector<uint8_t> &rom,
                   const std::vector<uint8_t> &file)
{
    const std::map<std::string, std::vector<uint8_t>> card = tb.files;
    g_resets_at_load = 0;
    g_card_kept = true;
    dut = new Vtb_pocket;
    bench_fresh();
    tb.bound[0] = "/Assets/rp6502/common/file.rp6502";
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

    std::vector<uint8_t> fw = fw_image();
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
        if (t.bridge)
            pw_put(r, BLOB_BRIDGE, file, t.wmode, t.wbase, service,
                   t.wshuffle != 0);
        else
            put_blob(file);
    };
    bool early = t.seq == SEQ_LOAD_WAIT_EXIT || t.seq == SEQ_LOAD_EXIT
                 || t.seq == SEQ_LOAD_BLOB_EXIT;
    bool quiet = t.service == 0;
    int want = early ? ANSWER_LOAD_IN_RESET : ANSWER_LOAD_RUNNING;
    bool fine = true;
    /* The Pocket that does not wait reads no answer, so none is held
     * against the core there. */
    bool heeds = t.seq != SEQ_LOAD_EXIT;
    auto ask = [&]() {
        g_resets_at_load = tb.resets;
        g_card_kept = tb.files == card;
        int code = sst_cmd(true, true, !quiet);
        if (code < 0 || (heeds && code != want))
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
        if (t.seq == SEQ_LOAD_WAIT_EXIT)
        {
            /* A poll every 2000 clocks of clk_74a, for a second at most. */
            int polls = 0;
            while (sst_cmd(true, false, false) != RES_OK && polls++ < 37000)
                for (int k = 0; k < 2000; k++)
                    a_edge();
            if (polls >= 37000)
            {
                fprintf(stderr, "  wake: the load never answered ok in "
                                "reset\n");
                return 2;
            }
        }
        if (t.seq == SEQ_LOAD_BLOB_EXIT)
            put(false);
        tb.stop_arm = true;
        pw_exit_reset();
    }
    else
    {
        if (t.drb < 0)
            put(false);
        pw_exit_reset();
        if (t.drb >= 0)
        {
            pw_run(t.drb, true);
            put(t.service == 1);
        }
        pw_run(t.dbl, !quiet);
        tb.stop_arm = true;
        tb.console.clear();
        fprintf(stderr, "  wake: at the load pocket_file busy=%d fstate=%d "
                        "op=%d, request waiting=%d, the fresh soft CPU at "
                        "%x\n",
                (int)PF(busy), (int)PF(fstate), (int)PF(r_op), tb.req, RVPC());
        ask();
    }
    if (!pw_load_over(early, quiet, heeds))
        return 2;
    bool refused = (int)MEM(engine__DOT__ld_bad);
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

/* The line the console shows for a wake whose blob is refused. */
static std::string pw_line(const Trip &)
{
    return "bad savestate ";
}

/* A refused wake says why on the console and goes no further: when the
 * line is up, the 6502 is in reset and the screen is the console's.
 * Returns what failed, or nothing. */
static const char *pw_held(const std::string &line)
{
    for (long i = 0; i < 60000000L; i++)
    {
        step();
        if ((i & 0xFFFF) == 0 && pw_screen().find(line) != std::string::npos)
            return (int)MEM(resb) || (uint32_t)MEM(prog__DOT__canvas_shadow)
                       ? "FAIL refused-not-held"
                       : nullptr;
    }
    return "FAIL refused-no-line";
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
                (int)dut->target_dataslot_done, tb.req);
    }
    std::string rv = tb.rv.size() > 400 ? tb.rv.substr(tb.rv.size() - 400)
                                       : tb.rv;
    fprintf(stderr,
            "  %s: resb=%d pc=%04x engine=%d clken=%d canvas=%u slot=%u "
            "console=[%s] rv=[%s]\n",
            tag, (int)MEM(resb), (unsigned)MEM(cpu__DOT__pc),
            (int)MEM(engine__DOT__state),
            (int)dut->rootp->tb_pocket__DOT__mach_clk_en,
            (unsigned)MEM(prog__DOT__canvas_shadow),
            (unsigned)MEM(soc__DOT__mmio_slot_len), tb.console.c_str(),
            rv.c_str());
}

/* What the picture program has made, as the video device holds it, and
 * where its 6502 is: the canvas, the terminal's scanline range, the vsync
 * line, the scanline program, XRAM, and the 6502's registers. */
static std::vector<uint32_t> pw_picture()
{
    std::vector<uint32_t> v;
    v.push_back((uint32_t)MEM(prog__DOT__canvas_shadow));
    v.push_back((uint32_t)MEM(mode0__DOT__prog_shadow));
    v.push_back((uint32_t)MEM(prog__DOT__vsync_shadow));
    for (int i = 0; i < 2048; i++)
    {
        v.push_back((uint32_t)MEM(prog__DOT__fill_e)[i]);
        v.push_back((uint32_t)MEM(prog__DOT__fill_c)[i]);
        v.push_back((uint32_t)MEM(prog__DOT__spr_e)[i]);
        v.push_back((uint32_t)MEM(prog__DOT__spr_c)[i]);
    }
    for (int i = 0; i < 16384; i++)
        v.push_back((uint32_t)MEM(xram__DOT__mem0)[i]
                    | (uint32_t)MEM(xram__DOT__mem1)[i] << 8
                    | (uint32_t)MEM(xram__DOT__mem2)[i] << 16
                    | (uint32_t)MEM(xram__DOT__mem3)[i] << 24);
    v.push_back((uint32_t)MEM(cpu__DOT__pc));
    v.push_back((uint32_t)MEM(cpu__DOT__a) | (uint32_t)MEM(cpu__DOT__x) << 8
                | (uint32_t)MEM(cpu__DOT__y) << 16
                | (uint32_t)MEM(cpu__DOT__s) << 24);
    v.push_back((uint32_t)MEM(cpu__DOT__p)
                | (uint32_t)MEM(cpu__DOT__stop_flag) << 8
                | (uint32_t)MEM(resb) << 9);
    return v;
}

/* Steps until the program has printed its last words, and a little
 * longer. */
static bool pw_to_last_words(int prog)
{
    const char *last = pw_last_words(prog);
    long i = 0;
    for (; i < 40000000L && tb.console.find(last) == std::string::npos; i++)
        step();
    for (long k = 0; k < 200000L; k++)
        step();
    return i < 40000000L;
}

static const std::string SAVE_FILE = "/Saves/rp6502/common/T.DAT";
static const std::string SAVE_DATA = "pocket file ok\r\n";

static bool pw_save_file_right()
{
    auto it = tb.files.find(SAVE_FILE);
    return it != tb.files.end()
           && std::string(it->second.begin(), it->second.end()) == SAVE_DATA;
}

/* The save was refused: the file program has its file open for writing,
 * or an open that may change the card under way. The host is told err and
 * stays told, no blob is served, and the machine runs on as if nothing had
 * been asked: its clock never stops, the soft CPU is never halted, and the
 * program ends with its file right. Once the file is closed a save is
 * taken again. */
static int pw_refused(Trip &t)
{
    const char *bad = nullptr;
    fprintf(stderr, "  the save was refused, with the card holding %s\n",
            tb.files.count(SAVE_FILE)
                ? (std::to_string(tb.files[SAVE_FILE].size()) + " bytes of "
                   "the file").c_str()
                : "no file yet");
    if (t.prog != 0 || !(int)SST(refuse_s2))
        bad = "FAIL refused-with-nothing-open";
    if (sst_cmd(false, false, true) != ANSWER_SAVE_REFUSED)
        bad = "FAIL refusal-forgotten";
    /* The host reads the window all the same, and gets no engine. */
    for (uint32_t i = 0; i < 32; i++)
    {
        pw_read_word(i, i + 1, 86);
        if ((int)MEM(engine__DOT__state) != 0
            || dut->tb_pocket_savestate_start_busy)
            bad = "FAIL refused-save-served";
    }
    if (!pw_to_last_words(0) || !pw_save_file_right())
        bad = "FAIL refused-session-wrong";
    if (!tb.stop_arm || !tb.halt_arm)
        bad = "FAIL refused-save-stopped-the-machine";
    if (sst_cmd(false, false, true) != ANSWER_SAVE_REFUSED)
        bad = "FAIL refusal-forgotten";
    /* The file is closed. */
    if (!bad)
    {
        if (sst_cmd(false, true, true) != ANSWER_SAVE || !save_wait())
            bad = "FAIL save-refused-after-the-close";
        else
        {
            abort_save();
            long guard = 0;
            while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
                step();
        }
    }
    if (tb.wild || tb.stray)
        bad = "FAIL wild-command";
    if (bad)
    {
        pw_print(t, bad);
        pw_state("B");
    }
    else
        pw_print(t, "ok refused");
    teardown();
    return bad != nullptr;
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
    /* PWAKE_TRAPFW runs the trip on the firmware that traps. */
    g_trap_fw = env_long("PWAKE_TRAPFW", 0) != 0;
    if (rom.empty() || !pw_find_trap())
        return 100;
    const long n = 1500;
    const char *last = pw_last_words(t.prog);
    /* A pinned trip is about one thing and must show it. A random one may
     * land where there is nothing to compare. */
    bool pinned = env_long("PWAKE_PINNED", 0) != 0;

    /* B: the session that sleeps. It plays on after the save, and what it
     * does then is what the wake has to do. */
    card_fresh();
    if (!boot_to_mark(rom, t.prog))
    {
        pw_print(t, "FAIL no-mark");
        teardown();
        return 101;
    }
    for (long i = 0; i < t.save_at; i++)
        step();
    /* PWAKE_SAVETRAP asks for it as the firmware enters its trap handler
     * for that many'th time from here. */
    if (long nth = env_long("PWAKE_SAVETRAP", 0))
    {
        int from = tb.traps;
        for (long i = 0; i < 40000000L && tb.traps < from + nth; i++)
            step();
        if (tb.traps < from + nth)
        {
            pw_print(t, "FAIL no-such-trap");
            teardown();
            return 104;
        }
    }
    /* PWAKE_SAVEOP_AFTER lets the 6502's API call of that number begin
     * first, and PWAKE_SAVEIDLE then lets it end. */
    if (long first = env_long("PWAKE_SAVEOP_AFTER", 0))
    {
        long i = 0;
        for (; i < 40000000L
               && !(api_busy() && MEM(ria__DOT__regs)[0x0F] == (uint8_t)first);
             i++)
            step();
        for (; env_long("PWAKE_SAVEIDLE", 0) && i < 40000000L && api_busy();
             i++)
            step();
        if (i >= 40000000L)
        {
            pw_print(t, "FAIL no-such-call");
            teardown();
            return 104;
        }
    }
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
    /* PWAKE_SAVE_D is so many steps more. */
    for (long i = env_long("PWAKE_SAVE_D", 0); i > 0; i--)
        step();
    if (last && tb.console.find(last) != std::string::npos)
    {
        pw_print(t, pinned ? "FAIL saved-after-done" : "skip saved-after-done");
        teardown();
        return pinned;
    }
    tb.stop_arm = true;
    tb.halt_arm = true;
    long asked = tb.sys;
    int resets_at_save = tb.resets, opens_at_save = tb.open_reqs;
    bool busy_at_save = api_busy();
    int op_at_save = MEM(ria__DOT__regs)[0x0F];
    /* PWAKE_REFUSED is whether the save must be refused, 1, or taken, 0. */
    long must = env_long("PWAKE_REFUSED", -1);
    int code = sst_cmd(false, true, true);
    if (code == ANSWER_SAVE_REFUSED && must != 0)
        return pw_refused(t);
    if (code != ANSWER_SAVE || must == 1 || !save_wait())
    {
        fprintf(stderr, "  the save request answered %d\n", code);
        pw_print(t, must == 1 ? "FAIL save-not-refused"
                              : "FAIL save-not-ready");
        pw_state("save");
        teardown();
        return 102;
    }
    long b_at = tb.stop_tick;
    /* The card as the sleep leaves it. */
    std::map<std::string, std::vector<uint8_t>> card = tb.files;
    fprintf(stderr, "  saved with the soft CPU halted at %x%s, %ld clocks "
                    "after the save was asked for, the 6502 %s, %d Open "
                    "Files sent\n", RVPC(),
            tb.halt_trap ? " in its trap handler" : "", tb.halt_sys - asked,
            busy_at_save ? "in an API call" : "between calls", tb.open_reqs);
    int bad = 0;
    /* A save is taken with no file open for writing, and with no command
     * sent yet of an open that may change the card. So a save taken as the
     * 6502 calls open halted the firmware before the call's first Open
     * File. */
    if ((int)SST(refuse_s2) && !(int)SST(start_hold))
    {
        pw_print(t, "FAIL saved-while-refusing");
        bad = 1;
    }
    if (busy_at_save && op_at_save == 0x14 && t.prog == 0
        && card.count(SAVE_FILE) == 0 && tb.open_reqs != opens_at_save)
    {
        pw_print(t, "FAIL saved-inside-a-creating-open");
        bad = 1;
    }
    if (env_long("PWAKE_SAVETRAP", 0) && !tb.halt_trap)
    {
        pw_print(t, "FAIL not-in-the-trap-handler");
        bad = 1;
    }
    std::vector<uint8_t> blob;
    bool rd_ok;
    if (t.bridge)
        rd_ok = pw_read_blob(r, blob, t.rmode, t.rbase, t.rorder);
    else
    {
        read_blob(blob);
        rd_ok = !dut->tb_pocket_savestate_start_err;
    }
    bool sum_ok = pw_blob_sum_ok(blob);
    if (!rd_ok || !sum_ok)
    {
        fprintf(stderr, "  save: underrun=%d sum_ok=%d\n", !rd_ok, sum_ok);
        pw_print(t, !sum_ok ? "FAIL blob-sum" : "NOTE underrun");
    }
    std::string b_console;
    std::vector<uint32_t> b_mix, b_picture;
    std::vector<Cpu> b_cpu;
    uint32_t b_canvas = 0;
    {
        long guard = 0;
        while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
            step();
        tb.console.clear();
        if (last)
            pw_to_last_words(t.prog);
        else if (t.prog == 1)
            run_ticks((size_t)(b_at + n));
        else
            for (long i = 0; i < 400000L; i++)
                step();
        b_console = tb.console;
        b_mix = tb.mix;
        b_cpu = tb.cpu;
        b_canvas = (uint32_t)MEM(prog__DOT__canvas_shadow);
        b_picture = pw_picture();
        if (!(int)MEM(resb) && !last)
        {
            pw_print(t, "FAIL save-session-dead");
            pw_state("B");
        }
        if (last && b_console.find(last) == std::string::npos)
        {
            pw_print(t, "FAIL save-session-never-ended");
            pw_state("B");
            bad = 1;
        }
    }
    /* What the session that slept did after its save, the wake has to do
     * and no more: so many 6502 resets, none for a program that only goes
     * on. */
    int b_resets = tb.resets - resets_at_save;
    std::map<std::string, std::vector<uint8_t>> b_files = tb.files;
    teardown();
    if (!sum_ok)
        return 103;

    std::vector<uint8_t> file = wrap_blob(blob, 596 + (size_t)t.head_pad,
                                          52764);

    tb.files = card;
    int res = pw_wake(r, t, rom, file);
    long c_at = tb.stop_tick;
    if (t.refuse == 0)
    {
        if (res != 0)
        {
            pw_print(t, res == 1 ? "FAIL good-blob-refused"
                                 : "FAIL load-stuck");
            pw_state("C");
            bad = 1;
        }
        else if (last && !g_card_kept && !pinned)
        {
            /* The blob came so late that the fresh firmware ran the
             * program, which wrote the card before the load. The restore
             * does not put a file back, so there is nothing to hold the
             * resumed program to. */
            pw_print(t, "skip the-fresh-program-wrote-the-card");
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
            if (last && !g_card_kept)
            {
                /* The card and the console the resumed program is held to
                 * have to be its own work. */
                pw_print(t, "FAIL the-fresh-program-wrote-the-card");
                bad = 1;
            }
            if (last)
            {
                pw_to_last_words(t.prog);
                if (tb.console != b_console)
                {
                    pw_print(t, "FAIL resume-console");
                    fprintf(stderr, "  B console=[%s]\n", b_console.c_str());
                    pw_state("C");
                    bad = 1;
                }
                else if (tb.files != b_files)
                {
                    pw_print(t, "FAIL resume-card");
                    for (auto &f : b_files)
                        if (!tb.files.count(f.first)
                            || tb.files[f.first] != f.second)
                            fprintf(stderr, "  %s: %zu bytes, the session "
                                            "that slept on left %zu\n",
                                    f.first.c_str(),
                                    tb.files.count(f.first)
                                        ? tb.files[f.first].size() : 0,
                                    f.second.size());
                    for (auto &f : tb.files)
                        if (!b_files.count(f.first))
                            fprintf(stderr, "  %s: only after the wake\n",
                                    f.first.c_str());
                    bad = 1;
                }
            }
            else if (t.prog == 1)
            {
                run_ticks((size_t)(c_at + n));
                if ((long)tb.mix.size() < c_at + n || c_at < 0
                    || first_diff(b_mix, b_at, tb.mix, c_at, n) >= 0
                    || first_cpu_diff(b_cpu, b_at, tb.cpu, c_at, n) >= 0)
                {
                    pw_print(t, "FAIL resume-ticks");
                    pw_state("C");
                    bad = 1;
                }
            }
            else
            {
                for (long i = 0; i < 400000L; i++)
                    step();
                if (!(int)MEM(resb)
                    || (uint32_t)MEM(prog__DOT__canvas_shadow) != b_canvas)
                {
                    pw_print(t, "FAIL resume-picture");
                    pw_state("C");
                    bad = 1;
                }
                /* The picture program draws and stops, so what it made
                 * and where it stopped are the same in both sessions. */
                std::vector<uint32_t> c_picture = pw_picture();
                if (t.prog == 2 && c_picture != b_picture)
                {
                    size_t at = 0;
                    while (c_picture[at] == b_picture[at])
                        at++;
                    pw_print(t, "FAIL resume-picture-differs");
                    fprintf(stderr, "  word %zu of the picture is %08x, the "
                                    "session that slept on left %08x\n", at,
                            c_picture[at], b_picture[at]);
                    bad = 1;
                }
            }
            /* A program that resumed went on from where it was. One the
             * wake started over would have taken a reset. */
            if (tb.resets - g_resets_at_load != b_resets)
            {
                pw_print(t, "FAIL resumed-by-starting-over");
                fprintf(stderr, "  the 6502 began %d resets after the load, "
                                "and %d after the save in the session that "
                                "slept on\n", tb.resets - g_resets_at_load,
                        b_resets);
                bad = 1;
            }
        }
    }
    else if (res != 1)
    {
        pw_print(t, res == 0 ? "FAIL bad-blob-loaded" : "FAIL load-stuck");
        pw_state("C");
        bad = 1;
    }
    else if (const char *why = pw_held(pw_line(t)))
    {
        pw_print(t, why);
        pw_state("C");
        bad = 1;
    }
    if (tb.wild || tb.stray)
    {
        pw_print(t, tb.wild ? "FAIL wild-command" : "FAIL stray-command");
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

/* One pinned trip. Every choice has a value here, and a case names the
 * ones it is about: the audio program, which does not start itself again
 * and is compared tick for tick, saved 100,000 steps after its mark, a
 * save that is taken, a steady host that answers file requests, Reset Exit
 * and then the blob 1,000 steps later, while the firmware boots, and the
 * load 1,000 steps after the blob. With the blob that early the fresh
 * firmware starts no program, so what the card and the console hold after
 * the wake is the resumed program's work. */
static int pw_case(std::initializer_list<const char *> kv)
{
    static const char *const base[] = {
        "PWAKE_PROG", "1", "PWAKE_REFUSE", "0", "PWAKE_DRB", "1000",
        "PWAKE_SERVICE", "1", "PWAKE_DBL", "1000", "PWAKE_RMODE", "0",
        "PWAKE_RORDER", "0", "PWAKE_WMODE", "0", "PWAKE_WSHUFFLE", "0",
        "PWAKE_FWBRIDGE", "0", "PWAKE_SAVEAT", "100000", "PWAKE_SEQ", "0",
        "PWAKE_HEADPAD", "0", "PWAKE_SAVEOP", "0", "PWAKE_SAVEOP_K", "1",
        "PWAKE_PINNED", "1", "PWAKE_SAVETRAP", "0", "PWAKE_SAVEOP_AFTER",
        "0", "PWAKE_SAVEIDLE", "0", "PWAKE_SAVE_D", "0", "PWAKE_REFUSED",
        "0", "PWAKE_TRAPFW", "0", "PWAKE_BRIDGE", "0"};
    std::vector<const char *> all(std::begin(base), std::end(base));
    all.insert(all.end(), kv.begin(), kv.end());
    for (size_t i = 0; i + 1 < all.size(); i += 2)
        setenv(all[i], all[i + 1], 1);
    Trip t{};
    t.seed = 1;
    int bad = pw_trip(t);
    for (size_t i = 0; i + 1 < all.size(); i += 2)
        unsetenv(all[i]);
    g_trap_fw = false;
    return bad;
}


/* The fresh firmware has staged the program and it runs when the blob
 * comes. A good blob resumes the program exactly. */
UTEST(pwake, the_blob_and_the_load_after_the_program_started_resume)
{
    ASSERT_EQ(0, pw_case({"PWAKE_DRB", "5000000"}));
}

/* The load arrives while the Pocket holds the core in reset, and the
 * Pocket polls for its ok before Reset Exit. */
UTEST(pwake, a_load_answered_in_reset_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "1"}));
}

/* A blob from another build. The console says so and the 6502 stays in
 * reset. */
UTEST(pwake, another_build_refused_in_reset_says_so_and_stops)
{
    ASSERT_EQ(0, pw_case({"PWAKE_SEQ", "1", "PWAKE_REFUSE", "1"}));
}

/* No savestate while a file is open for writing. The file program opens
 * its file for writing, and a save asked for between the open's return
 * and the write, with the 6502 in no call, is answered err at the request.
 * Nothing is saved and the program ends with its file right. */
UTEST(pwake, a_save_between_calls_on_a_file_open_for_writing_is_refused)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP_AFTER", "0x14", "PWAKE_SAVEIDLE", "1",
                          "PWAKE_REFUSED", "1"}));
}

/* A file open only for reading refuses nothing. The save comes in the
 * read, with the read sent, and the read is made again after the wake: the
 * program prints what it read. */
UTEST(pwake, a_save_in_a_read_that_was_sent_resumes)
{
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "0", "PWAKE_SAVEAT", "0",
                          "PWAKE_SAVEOP", "0x16", "PWAKE_SAVEOP_K", "5",
                          "PWAKE_SEQ", "1"}));
}


/* The firmware emulates a misaligned access in its trap handler, and the
 * save halts it there. The handler returns through mepc, which the fresh
 * firmware of the wake has never set, so the program goes on only if the
 * blob carried it. The trap program fails an API call over and over, and
 * on the firmware that traps every failure's errno is such an access. */
UTEST(pwake, a_save_inside_the_trap_handler_resumes)
{
    /* As it saves the registers, and as it performs the access. */
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "3", "PWAKE_TRAPFW", "1",
                          "PWAKE_SAVEAT", "50000", "PWAKE_SAVETRAP", "3",
                          "PWAKE_SAVE_D", "40", "PWAKE_SEQ", "1"}));
    ASSERT_EQ(0, pw_case({"PWAKE_PROG", "3", "PWAKE_TRAPFW", "1",
                          "PWAKE_SAVEAT", "50000", "PWAKE_SAVETRAP", "5",
                          "PWAKE_SAVE_D", "400", "PWAKE_SEQ", "1"}));
}
