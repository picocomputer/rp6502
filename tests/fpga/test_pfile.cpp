/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The file round trip, with the bench playing the host. A .rp6502
 * writes a file through the 6502's syscalls, closes it, opens it
 * again, reads it back and prints it; the host side here answers the
 * APF target commands the way a Pocket does — Open File takes a name
 * out of the core's own window and binds a slot to a std::vector,
 * Slot Read streams that vector into the staging store over the
 * bridge, Slot Write reads it back out of the window, and the data
 * table reports the length. If the printed bytes match what the
 * program wrote, then the whole path held: std.c's dispatch, msc.c's
 * driver, pocket_file's crossing, and both directions through a
 * bridge that is not symmetric.
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static Vtb_pocket *dut;
static long a_next, s_next, g_sys;
static uint32_t dt_pipe[2];
/* The host's id/size table, as pairs. */
static uint32_t g_dt[64];

/* Where msc.c stages a Slot Read, and where the host reads a Slot
 * Write out of. Both are firmware and RTL constants; a test that
 * guessed them would pass against the wrong hardware. */
static const uint32_t STAGE_BRIDGE = 0x03FE0000u;
static const uint32_t WINDOW_BASE = 0x20000000u;

/* The host's filesystem, and which slot is bound to which file. */
static std::map<std::string, std::vector<uint8_t>> g_files;
static std::string g_bound[16];
static std::string g_console;
static int g_opens, g_reads, g_writes, g_flushes;

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
        /* mf_datatable answers two clk_74a later than the address: an
         * address register and an output register, both on CLOCK0.
         * Driving datatable_q combinationally let a core that captured
         * a clock early pass here, and on hardware that core read the
         * address the loader leaves standing — a constant 1 — so every
         * slot came back holding slot 0's size. */
        dut->datatable_q = dt_pipe[1];
        dt_pipe[1] = dt_pipe[0];
        dt_pipe[0] = g_dt[dut->tb_pocket_dt_addr & 63];
        dut->clk_74a = 1;
    }
    dut->eval();
    /* Once per machine clock: the valid is a level for that clock and
     * the bridge's edges fall inside it. */
    if (sedge && dut->tb_pocket_tx_valid)
        g_console += (char)dut->tb_pocket_tx_data;
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

/* --- The bridge, from the host's end --- */

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

/* A read is a strobe, and the word it names is held until the next one:
 * "the core may not immediately provide the read data and has up until
 * the next read strobe to drive bridge_rd_data". The bench used to set
 * an address and sample, never pulsing bridge_rd at all, so a core that
 * simply chased bridge_addr passed here and handed hardware the word
 * after the one it was asked for. */
static uint32_t host_read(uint32_t addr)
{
    dut->bridge_addr = addr;
    a_edge();
    dut->bridge_rd = 1;
    a_edge();
    dut->bridge_rd = 0;
    /* The host moves the address on to the next word before it takes
     * this one — that is what the buffering buys it. Holding the address
     * still here is what let a core that chases bridge_addr pass, and
     * then hand hardware the word after the one it asked for. */
    dut->bridge_addr = addr + 4;
    for (int k = 0; k < 6; k++)
        a_edge();
    return dut->tb_pocket_bridge_rd_data;
}

/* Byte zero of a word rides the top eight bits: bridge_endian_little
 * is clear, and the ROM loader has depended on that since the first
 * image landed. */
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

/* --- The data table --- */


static void dt_set(uint32_t slot, uint32_t size)
{
    g_dt[slot * 2] = slot;
    g_dt[slot * 2 + 1] = size;
}

/* --- Target commands --- */

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
    /* Either form resolves here. Which one the real host accepts is the
     * open question — Analogue documents a full path, PocketQuake passes
     * one relative to Assets — so the bench takes both and hardware is
     * the arbiter. Asserting a form here would only enshrine a guess. */
    const std::string root = "/Saves/rp6502/common/";
    if (name.rfind(root, 0) == 0)
        name = name.substr(root.size());
    /* The struct's integers are bridge words, not bytes of the stream the
     * path rides in. The bench had them the other way round and so agreed
     * with a firmware that wrote them reversed, which is how a create
     * that never once worked on hardware kept a green suite: the host
     * saw flags of 3 as 0x03000000 and opened without creating. Read
     * them the way the real host does, and put the byte order back in
     * msc_win_u32 to watch this test go red. */
    uint32_t flags = ((uint32_t)param[256] << 24) | ((uint32_t)param[257] << 16)
                     | ((uint32_t)param[258] << 8) | (uint32_t)param[259];
    uint32_t size = ((uint32_t)param[260] << 24) | ((uint32_t)param[261] << 16)
                    | ((uint32_t)param[262] << 8) | (uint32_t)param[263];
    g_opens++;
    bool created = false;
    auto it = g_files.find(name);
    /* Zero-length files are allowed here, and the reason that is not
     * obvious: they were once refused, because every create the machine
     * had ever made came back "not found" and a length of zero looked
     * like the cause. It was not — the flags word was arriving swapped
     * and the host was seeing no create bit at all. Whether the real
     * host will hold a file at zero is a hardware question; do not put
     * a refusal back here on the strength of the old runs, because
     * every one of them was made through that bug. */
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
        /* Create takes both bits. Bit 0 on its own is answered with a
         * descriptor and makes nothing: measured, eight opens asking for
         * O_CREAT without O_TRUNC each came back a handle and none of
         * them left a file. Resize is what puts it there, so a create
         * without it succeeds loudly and does nothing at all. */
        if (!(flags & 2))
        {
            dut->target_dataslot_done = 1;
            dut->target_dataslot_err = 1; /* created and opened, it says */
            for (int k = 0; k < 4; k++)
                a_edge();
            return;
        }
        it = g_files.emplace(name, std::vector<uint8_t>()).first;
        created = true;
    }
    if (flags & 2)
        it->second.resize(size, 0);
    g_bound[slot] = name;
    dt_set(slot, (uint32_t)it->second.size());
    /* 0 opened, 1 created and opened; the host tells them apart and only
     * 2 and up are failures. */
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = created ? 1 : 0;
    for (int k = 0; k < 4; k++)
        a_edge();
}

static void do_slotread()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t off = dut->tb_pocket_ds_slotoffset;
    uint32_t len = dut->tb_pocket_ds_length;
    uint32_t at = dut->tb_pocket_ds_bridgeaddr;
    g_reads++;
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

/* Flush commits a slot to the card. Nothing here buffers, so the answer
 * is simply yes — but it has to be answered, because a command the host
 * never retires leaves the machine waiting out its timeout. */
static void do_flush()
{
    dut->target_dataslot_done = 0;
    g_flushes++;
    /* The core proves its command was taken by watching done fall before
     * it rises again, so the fall has to last long enough to be seen.
     * Every other handler gets that for free from the clocks its host
     * memory access burns; this one touches nothing and would otherwise
     * hand back an answer to a question the core never saw asked. */
    for (int k = 0; k < 4; k++)
        a_edge();
    target_done();
}

/* One clk_sys step with the host watching for a command. The request
 * lines are one pulse wide, so this samples every clock. */
static void step()
{
    static int prev_r, prev_w, prev_o, prev_f;
    tick();
    int r = dut->tb_pocket_ds_read, w = dut->tb_pocket_ds_write,
        o = dut->tb_pocket_ds_openfile, f = dut->tb_pocket_ds_flush;
    if (o && !prev_o)
    {
        prev_r = prev_w = prev_o = prev_f = 0;
        do_openfile();
        return;
    }
    if (f && !prev_f)
    {
        prev_r = prev_w = prev_o = prev_f = 0;
        do_flush();
        return;
    }
    if (r && !prev_r)
    {
        prev_r = prev_w = prev_o = prev_f = 0;
        do_slotread();
        return;
    }
    if (w && !prev_w)
    {
        prev_r = prev_w = prev_o = prev_f = 0;
        do_slotwrite();
        return;
    }
    prev_r = r;
    prev_w = w;
    prev_o = o;
    prev_f = f;
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

static void boot(const std::vector<uint8_t> &rom)
{
    dut = new Vtb_pocket;
    a_next = s_next = g_sys = 0;
    g_console.clear();
    g_files.clear();
    g_opens = g_reads = g_writes = g_flushes = 0;
    memset(g_dt, 0, sizeof g_dt);
    for (auto &b : g_bound)
        b.clear();

    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
    dut->dataslot_allcomplete = 0;
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = 0;
    for (int i = 0; i < 40; i++)
        tick();

    auto *r = dut->rootp;
    tb_load_tcm(r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm0,
                r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm1,
                r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm2,
                r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm3,
                SW_BIN);

    dut->rst_n = 1;
    dut->arst_n = 1;
    /* The store wakes on a 200 us JEDEC count and the bridge's queue is
     * eight deep, so a load that starts before it is up overflows. */
    for (int i = 0; i < 40000 && !dut->tb_pocket_ready; i++)
        tick();

    /* The host's load: the ROM into slot 0, the fonts where data.json
     * puts them, then the table and completion. */
    host_put_bytes(0, rom.data(), rom.size());
    std::vector<uint8_t> fonts = read_file(FONTS_BIN);
    host_put_bytes(0x03FF0000u, fonts.data(), fonts.size());
    std::vector<uint8_t> oemcp = read_file(OEMCP_BIN);
    host_put_bytes(0x03FD0000u, oemcp.data(), oemcp.size());
    dt_set(0, (uint32_t)rom.size());
    dut->datatable_q = g_dt[1];
    dut->dataslot_allcomplete = 1;
    dut->reset_n = 1;
    for (int i = 0; i < 4000; i++)
        tick();
}

static void teardown()
{
    dut->final();
    delete dut;
    dut = nullptr;
}

UTEST_MAIN();

UTEST(pfile, a_program_writes_a_file_and_reads_it_back)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);

    const std::string want = "pocket file ok\r\n";
    for (long i = 0; i < 40000000L && g_console.find(want) == std::string::npos;
         i++)
        step();

    if (g_console.find(want) == std::string::npos)
        fprintf(stderr, "console: [%s] opens=%d reads=%d writes=%d flushes=%d\n",
                g_console.c_str(), g_opens, g_reads, g_writes, g_flushes);
    ASSERT_TRUE(g_console.find(want) != std::string::npos);

    /* The host's own copy is the other half of the proof: the bytes
     * reached a file, not just a buffer the machine still owns. */
    ASSERT_EQ(g_files.size(), (size_t)1);
    const std::vector<uint8_t> &f = g_files.begin()->second;
    ASSERT_EQ(f.size(), want.size());
    ASSERT_EQ(memcmp(f.data(), want.data(), want.size()), 0);
    ASSERT_GT(g_writes, 0);
    ASSERT_GT(g_reads, 0);

    teardown();
}

/* The whole drive in one boot, run here first. Every check the ROM makes
 * is one the machine decides for itself, so the bench can hold it to the
 * same standard the card does: all twenty-six, or name the ones that
 * failed. A ROM shipped without this costs a bitstream and a photograph
 * to find a branch that went the wrong way. */
UTEST(pfile, the_whole_drive_conforms)
{
    std::vector<uint8_t> rom = read_file(FSTEST_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);

    for (long i = 0; i < 60000000L && g_console.find("BAD") == std::string::npos;
         i++)
        step();

    size_t at = g_console.find("BAD");
    if (at == std::string::npos)
        fprintf(stderr, "console: [%s] opens=%d reads=%d writes=%d\n",
                g_console.c_str(), g_opens, g_reads, g_writes);
    ASSERT_TRUE(at != std::string::npos);

    /* 26 checks, printed in hex. Anything less and the console names
     * which ones on the BAD line. */
    if (g_console.find("PASS 1A/1A") == std::string::npos)
        fprintf(stderr, "console: [%s]\n", g_console.c_str());
    ASSERT_TRUE(g_console.find("PASS 1A/1A") != std::string::npos);
}
