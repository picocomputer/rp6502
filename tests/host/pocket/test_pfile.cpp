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
 *
 * THE HOST MODELLED HERE IS OURS, NOT ANALOGUE'S. Analogue publishes
 * the command numbers and the register layout and almost nothing about
 * behaviour, so every answer this file gives back — which result code
 * means what, when the folder has to already exist, the byte order of
 * the parameter struct's integers, the shape of Get File's response —
 * was reverse engineered by poking a real Pocket and writing down what
 * it did. Some of it is certainly wrong, and a firmware update can
 * make more of it wrong with nothing anywhere saying so. A green run
 * here means the core agrees with our model of the host. Only hardware
 * says whether the model is right.
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
/* The host's id/size table, as pairs. */
static uint32_t g_dt[64];

/* The host's filesystem, and which slot is bound to which file. Files
 * are keyed by their absolute card path, folders modeled because the
 * real host will not create one: a create into a folder that is not
 * there answers with a descriptor and writes nothing. */
static std::map<std::string, std::vector<uint8_t>> g_files;
static std::set<std::string> g_dirs;
static std::string g_bound[16];
static std::string g_console;
static std::string g_rv;
static int g_opens, g_reads, g_writes, g_flushes, g_getfiles;

/* The host's one-deep command queue, standing in for the framework's.
 * Latched by tick(), served by step(); g_servicing keeps a handler's
 * own clocking out of its own reentry. A request line is one clk_74a
 * wide, so sampling it only on the clocks a test steps drops any
 * command raised while the host was busy driving the bridge. */
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
    if (sedge && dut->tb_pocket_rv_tx_valid)
        g_rv += (char)dut->tb_pocket_rv_tx_data;

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

/* The real host takes milliseconds to answer: the bridge moves one word
 * per ~1180ns and the card behind it costs more than the transfer. A
 * handler that answers within its own few cycles is a host the firmware
 * never has to share the machine with, and against it every driver bug
 * that needs a second worker running while a command is outstanding is
 * invisible. This is a tenth of a millisecond -- short for a card, and
 * long enough that the main loop goes round many times inside one
 * command. */
static long g_host_delay = 5000;

static void host_wait(long n)
{
    while (n-- > 0)
        tick();
}

static void target_answer(int err)
{
    host_wait(g_host_delay);
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = err;
    for (int k = 0; k < 4; k++)
        a_edge();
}

static void target_done()
{
    target_answer(0);
}

static void do_openfile()
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint8_t param[264];
    host_get_bytes(dut->tb_pocket_param_struct, param, sizeof param);
    std::string name((const char *)param);
    /* The host resolves nothing. A name reaches it as an absolute path
     * or not at all — measured, when the same run wrote 004.bin spelled
     * in full and never produced 000.bin spelled bare. Refusing the
     * relative form here is what holds the drive to spelling it out. */
    if (name.empty() || name[0] != '/')
    {
        g_opens++;
        target_answer(4); /* malformed path */
        return;
    }
    std::string key = name;
    std::string parent = key.substr(0, key.rfind('/'));
    if (parent.empty())
        parent = "/";
    /* The struct's integers are bridge words, not bytes of the stream the
     * path rides in. The bench had them the other way round and so agreed
     * with a firmware that wrote them reversed, which is how a create
     * that never once worked on hardware kept a green suite: the host
     * saw flags of 3 as 0x03000000 and opened without creating. Read
     * them the way the real host does, and put the byte order back in
     * fs_win_u32 to watch this test go red. */
    uint32_t flags = ((uint32_t)param[256] << 24) | ((uint32_t)param[257] << 16)
                     | ((uint32_t)param[258] << 8) | (uint32_t)param[259];
    uint32_t size = ((uint32_t)param[260] << 24) | ((uint32_t)param[261] << 16)
                    | ((uint32_t)param[262] << 8) | (uint32_t)param[263];
    g_opens++;
    bool created = false;
    auto it = g_files.find(key);
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
            target_answer(3); /* file not found */
            return;
        }
        /* Create takes both bits. Bit 0 on its own is answered with a
         * descriptor and makes nothing: measured, eight opens asking for
         * O_CREAT without O_TRUNC each came back a handle and none of
         * them left a file. Resize is what puts it there, so a create
         * without it succeeds loudly and does nothing at all — and so
         * does a create into a folder the card does not have, which is
         * the hollow answer the firmware's conjure exists for. */
        if (!(flags & 2) || !g_dirs.count(parent))
        {
            target_answer(1); /* created and opened, it says */
            return;
        }
        it = g_files.emplace(key, std::vector<uint8_t>()).first;
        created = true;
    }
    if (flags & 2)
        it->second.resize(size, 0);
    g_bound[slot] = key;
    dt_set(slot, (uint32_t)it->second.size());
    /* 0 opened, 1 created and opened; the host tells them apart and only
     * 2 and up are failures. */
    target_answer(created ? 1 : 0);
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
 * never retires leaves the machine waiting out its deadline. */
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

/* Get File, the only command that answers with more than a code: the
 * host writes back the name bound to the slot. It lands wherever the
 * response struct points, which for this core is the scratch between
 * the assets and the ROM, because the bridge writes only the store.
 *
 * Analogue documents the command and not the shape of the answer. A
 * NUL-terminated name at offset 0 is what Open File's parameter struct
 * carries and what the firmware reads back; the real host is what
 * settles whether that is right. */
/* Every Get File the firmware asks for, and whether the fabric noticed
 * the answer. The bit is set while the command is still outstanding, so
 * it is read after the completion this function hands back. */
static int g_getfile_seen;
static int g_getfile_wrote;

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
    /* The flag rides the completion handshake into clk_sys, so it is not
     * there the instant done goes back up. */
    for (int k = 0; k < 64; k++)
        a_edge();
    g_getfile_seen++;
    if (dut->rootp->tb_pocket__DOT__core__DOT__file__DOT__wrote_flag)
        g_getfile_wrote++;
}

/* One clk_sys step with the host watching for a command. The request
 * lines are one pulse wide, so this samples every clock. */
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

/* The card as installed carries every folder the package ships, drive
 * included — the host makes none of them. `homeless` is the card that
 * lost its Saves folder, where the drive can only fail, and must fail
 * cleanly rather than hang. */
static void boot(const std::vector<uint8_t> &rom, bool homeless)
{
    dut = new Vtb_pocket;
    a_next = s_next = g_sys = 0;
    g_console.clear();
    g_rv.clear();
    g_files.clear();
    g_dirs = {"/", "/Assets", "/Assets/rp6502", "/Assets/rp6502/common",
              "/Saves"};
    if (!homeless)
    {
        g_dirs.insert("/Saves/rp6502");
        g_dirs.insert("/Saves/rp6502/common");
    }
    g_opens = g_reads = g_writes = g_flushes = g_getfiles = 0;
    memset(g_dt, 0, sizeof g_dt);
    for (auto &b : g_bound)
        b.clear();
    /* Slot 0 is the ROM the user picked, bound before the core ever
     * runs — which is the whole reason argv has to ask. In the assets
     * folder, spelled absolute, the way the host answers. */
    g_bound[0] = "/Assets/rp6502/common/pfile.rp6502";
    /* And the file behind it, for the slot reads an exec pull makes. */
    g_files[g_bound[0]] = rom;
    g_dirs.insert("/Assets");
    g_dirs.insert("/Assets/rp6502");
    g_dirs.insert("/Assets/rp6502/common");

    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
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
    /* The store wakes on a 200 us JEDEC count and the bridge's queue is
     * eight deep, so a load that starts before it is up overflows. */
    for (int i = 0; i < 40000 && !dut->tb_pocket_ready; i++)
        tick();

    /* The host's load, whole files where data.json puts them: the fonts,
     * the code page tables, and every byte of the ROM, then the table
     * and completion. */
    std::vector<uint8_t> fonts = read_file(FONTS_BIN);
    host_put_bytes(TB_STAGE_FONT_BASE, fonts.data(), fonts.size());
    std::vector<uint8_t> oemcp = read_file(OEMCP_BIN);
    host_put_bytes(TB_STAGE_OEMCP_BASE, oemcp.data(), oemcp.size());
    host_put_bytes(TB_STAGE_ROM_BASE, rom.data(), rom.size());
    dt_set(0, (uint32_t)rom.size());
    /* The host's clock, local time behind the valid, as command 0x0090
     * leaves it: 2001-09-09 01:46:40, a billion seconds. */
    dut->rtc_epoch = 1000000000u;
    dut->rtc_valid = 1;
    /* The menu's UTC offset, five and a half hours east. Three entries
     * now, because a list holds sixteen options and the offset spans
     * twenty-seven hours: hours, quarter hour, and which side. A half
     * hour in it so a sign error cannot hide behind a whole-hour
     * symmetry, and non-zero so the two clocks must disagree. */
    host_write(0x1000000Cu, 5);   /* hours */
    host_write(0x10000010u, 30);  /* minutes */
    host_write(0x10000014u, 0);   /* east */
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
    boot(rom, false);

    const std::string want = "pocket file ok\r\n";
    for (long i = 0; i < 40000000L && g_console.find(want) == std::string::npos;
         i++)
        step();

    if (g_console.find(want) == std::string::npos)
        fprintf(stderr,
                "console: [%s] opens=%d reads=%d writes=%d flushes=%d "
                "getfiles=%d\n",
                g_console.c_str(), g_opens, g_reads, g_writes, g_flushes,
                g_getfiles);
    ASSERT_TRUE(g_console.find(want) != std::string::npos);

    /* The host's own copy is the other half of the proof: the bytes
     * reached a file, not just a buffer the machine still owns. The ROM
     * is a file on the card too, so count what the program left. */
    size_t made = 0;
    const std::vector<uint8_t> *f = NULL;
    for (std::map<std::string, std::vector<uint8_t>>::const_iterator it
             = g_files.begin();
         it != g_files.end(); ++it)
        if (it->first != g_bound[0])
        {
            made++;
            f = &it->second;
        }
    ASSERT_EQ(made, (size_t)1);
    ASSERT_EQ(f->size(), want.size());
    ASSERT_EQ(memcmp(f->data(), want.data(), want.size()), 0);
    ASSERT_GT(g_writes, 0);
    ASSERT_GT(g_reads, 0);
    /* Exactly one, and which one matters. The ROM closes twice and syncs
     * never, so this is the writable close flushing and the read-only
     * close declining to. There is no close command to send the host, so
     * a close that does not flush is a write left in the air. */
    ASSERT_EQ(g_flushes, 1);
    /* argv[0] is asked for once, before the 6502 is released. A core
     * that stopped asking would still pass everything above it and
     * would have no idea what it was running. */
    ASSERT_EQ(g_getfiles, 1);

    teardown();
}

/* The whole drive in one boot. Every check the ROM makes is one the
 * machine decides for itself, so the bench can hold it to the same
 * standard the card does: all forty-eight, or name the ones that
 * failed. A ROM shipped without this costs a bitstream and a photograph
 * to find a branch that went the wrong way. */
static void run_fstest(int *utest_result)
{
    /* Run to the end of the report, not to the word BAD: the failing
     * indices are printed after it and a newline closes the line.
     * Stopping at BAD threw away the only thing that says what broke. */
    size_t at = std::string::npos;
    for (long i = 0; i < 60000000L; i++)
    {
        step();
        if (at == std::string::npos)
            at = g_console.find("BAD");
        else if (g_console.find('\n', at) != std::string::npos)
            break;
    }

    if (at == std::string::npos)
        fprintf(stderr, "console: [%s] opens=%d reads=%d writes=%d\n",
                g_console.c_str(), g_opens, g_reads, g_writes);
    ASSERT_TRUE(at != std::string::npos);

    /* 48 checks, printed in hex. Anything less and the console names
     * which ones on the BAD line. */
    if (g_console.find("PASS 30/30") == std::string::npos)
        fprintf(stderr, "console: [%s]\n", g_console.c_str());
    ASSERT_TRUE(g_console.find("PASS 30/30") != std::string::npos);
}

UTEST(pfile, the_whole_drive_conforms)
{
    std::vector<uint8_t> rom = read_file(FSTEST_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom, false);
    run_fstest(utest_result);
    teardown();
}

/* The card that lost the drive's folder. The host will not create one
 * — a create into a path that is not there is answered with a
 * descriptor and writes nothing — so every call here can only fail.
 * What it must not do is take its time about it. The folder ships in
 * the package for exactly this reason; before it did, the firmware
 * tried to conjure it mid-session by dirtying and flushing a
 * nonvolatile slot, which on hardware bought nothing and cost seconds
 * of unanswered commands on every open. */
UTEST(pfile, a_card_without_the_drives_folder_fails_promptly)
{
    std::vector<uint8_t> rom = read_file(FSTEST_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom, true);

    for (long i = 0; i < 60000000L && g_console.find("PASS") == std::string::npos;
         i++)
        step();

    /* It reached its own tally instead of stalling inside an open. */
    if (g_console.find("PASS") == std::string::npos)
        fprintf(stderr, "console: [%s]\n", g_console.c_str());
    ASSERT_TRUE(g_console.find("PASS") != std::string::npos);
    /* And left nothing behind on a card that cannot hold it — the ROM
     * itself excepted, which was there before the core ran. */
    size_t made = 0;
    for (std::map<std::string, std::vector<uint8_t>>::const_iterator it
             = g_files.begin();
         it != g_files.end(); ++it)
        if (it->first != g_bound[0])
            made++;
    ASSERT_EQ(made, (size_t)0);
    teardown();
}

/* The name the host gave us, read back by the program it names.
 *
 * argv[0] on this machine has one source: Get File on the ROM slot. The
 * host answers with a 256-byte struct written into the response window
 * -- every time, blanked to a leading NUL when a slot is bound to
 * nothing, which is what tells a bound slot from an empty one. The
 * firmware used to ask the fabric whether any write had landed instead
 * of reading what the window said, and on hardware that flag stayed
 * clear while the right path sat in the window: nine calls in ten
 * discarded, argv empty, and a wake unable to recognise the ROM it was
 * already running.
 *
 * Empty brackets are that bug. The path is the fix. */
UTEST(pfile, the_program_is_told_what_it_is_called)
{
    std::vector<uint8_t> rom = read_file(ARGV_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom, false);

    for (long i = 0; i < 60000000L && g_console.find("]") == std::string::npos;
         i++)
        step();

    if (g_console.find(".rp6502") == std::string::npos)
        fprintf(stderr, "console: [%s]\n", g_console.c_str());
    ASSERT_TRUE(g_console.find(".rp6502") != std::string::npos);
    /* And it is the slot's own name, not a leftover from an earlier ask. */
    ASSERT_TRUE(g_console.find(g_bound[0]) != std::string::npos);
    teardown();
}

/* A Get File the host answers must be seen to have been answered.
 *
 * The fabric raises a bit when the host writes into the response window
 * while a Get File is outstanding, and the firmware once refused any
 * answer that arrived without it. On hardware that bit fired on the
 * first Get File after power-on and on none of the ninety that
 * followed: gf_pend was armed a state late, in F_ARM, which is a spin
 * that holds until the previous command's done falls and which
 * re-latched the arming from a request line it had cleared in its own
 * first cycle. Only the very first command escaped, done being 0 out of
 * reset -- so every later ask went unattributed and every name the host
 * gave was thrown away.
 *
 * Be clear about what this case does and does not do. It proves the bit
 * is raised for a Get File that was answered, which is a total-failure
 * net. It does NOT reproduce the one-shot, because provoking a second
 * Get File needs the firmware to stage twice and this harness has no
 * reload: dropping and re-settling dataslot_allcomplete here does not
 * bring main_stage back round, and test_pocket.cpp is the bench that
 * owns that sequence. Moving this there, or teaching this one to
 * reload, is what would close it. */
UTEST(pfile, every_get_file_is_seen_to_be_answered)
{
    std::vector<uint8_t> rom = read_file(ARGV_ROM);
    ASSERT_GT(rom.size(), 0u);
    g_getfile_seen = 0;
    g_getfile_wrote = 0;
    boot(rom, false);

    for (long i = 0; i < 30000000L && g_getfile_seen < 1; i++)
        step();


    /* Two is the whole point: one proves nothing, since one is what the
     * broken fabric managed. */
    if (g_getfile_wrote != g_getfile_seen)
        fprintf(stderr, "seen=%d wrote=%d console=[%s]\n", g_getfile_seen,
                g_getfile_wrote, g_console.c_str());
    ASSERT_GE(g_getfile_seen, 1);
    ASSERT_EQ(g_getfile_wrote, g_getfile_seen);
    teardown();
}

/* A program reading its own file, start to end, against the ownership
 * bookkeeping in fs.c.
 *
 * The fabric carries one command and answers it in one register. With
 * the 6502 parked in a syscall its operation is the only one in flight,
 * so this is the case where a poll can only ever find its own answer —
 * the control the drive's other tests are read against.
 *
 * The ROM checks its own reads, so this only has to run it and listen. */
UTEST(pfile, a_read_is_not_answered_by_someone_elses_command)
{
    std::vector<uint8_t> rom = read_file(SLEEPFILE_ROM);
    boot(rom, false);
    /* Whole units of the pattern the ROM expects, which it re-reads
     * from the top forever. */
    std::vector<uint8_t> dat;
    for (int u = 0; u < 64; u++)
        for (const char *c = "0123456789ABCDEF"; *c; c++)
            dat.push_back((uint8_t)*c);
    g_files["/Saves/rp6502/common/probe.dat"] = dat;

    for (long i = 0; i < 40000000L && g_console.size() < 4000; i++)
        step();

    if (g_console.find("CROOKED") != std::string::npos
        || g_console.find("FAILED") != std::string::npos
        || g_console.find("counting") == std::string::npos)
        fprintf(stderr, "console: [%s]\n", g_console.c_str());
    ASSERT_TRUE(g_console.find("counting") != std::string::npos);
    ASSERT_TRUE(g_console.find("CROOKED") == std::string::npos);
    ASSERT_TRUE(g_console.find("FAILED") == std::string::npos);
    teardown();
}
