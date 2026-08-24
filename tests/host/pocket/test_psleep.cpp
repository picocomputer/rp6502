/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Sleep, end to end, with the reconfigure in it.
 *
 * The Pocket does not suspend a core; it takes a savestate and cuts the
 * power, and on wake it loads a fresh bitstream and hands the blob
 * back. Every memory and every register comes back out of that
 * bitstream, which is why nothing short of a real savestate can carry a
 * running machine across one. So the bench does the whole thing
 * honestly: it boots a machine, marks every memory the blob claims to
 * carry, asks the host command for a state and reads the blob out over
 * the bridge at the documented one access per eighty-eight clocks --
 * then destroys the model.
 *
 * The second model is the wake. A new device with a cold store, the
 * host streaming its slots again, the blob written into the window as
 * ordinary bridge writes before any command arrives, and then 0x00A4.
 * If the marks are all there afterwards and the console never printed a
 * second boot, then the machine that came back is the machine that went
 * away.
 *
 * THE HOST MODELLED HERE IS OURS, NOT ANALOGUE'S -- the same caveat as
 * every other bench on this board. The order a wake does things in is
 * the part of it least likely to be right, and the one the device's own
 * debug log will settle.
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
 * own clocking out of its own reentry. */
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
    /* The request lines are one clk_74a wide -- pocket_file raises one
     * for a single cycle and the real framework latches it into a queue
     * -- so they have to be watched on every clock this model advances,
     * not only on the ones a test steps. A command raised while the
     * host was busy writing a data slot used to be dropped on the floor,
     * which is a bench that hangs where the device does not. */
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
    /* The host resolves nothing. A name reaches it as an absolute path
     * or not at all — measured, when the same run wrote 004.bin spelled
     * in full and never produced 000.bin spelled bare. Refusing the
     * relative form here is what holds the drive to spelling it out. */
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
         * without it succeeds loudly and does nothing at all — and so
         * does a create into a folder the card does not have, which is
         * the hollow answer the firmware's conjure exists for. */
        if (!(flags & 2) || !g_dirs.count(parent))
        {
            dut->target_dataslot_done = 1;
            dut->target_dataslot_err = 1; /* created and opened, it says */
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
    /* 0 opened, 1 created and opened; the host tells them apart and only
     * 2 and up are failures. */
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = created ? 1 : 0;
    for (int k = 0; k < 4; k++)
        a_edge();
}

/* A slot the host has not bound to a file is not a slot it can serve.
 * Answering zeros instead is a bench that passes a core which lost its
 * bindings, which is the whole of what a wake does to them. */
static bool slot_bound(uint32_t slot)
{
    if (slot < 16 && !g_bound[slot].empty())
        return true;
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = 1; /* slot not defined */
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

/* One clk_sys step, and then whatever command the model caught -- here
 * rather than in tick(), because a handler clocks the model itself and
 * must not be entered from inside its own clocking. */
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


/* Where the host reads and writes a state. core_top declares the same
 * address and the same length, and stage_map_gate holds them together
 * with the engine's own word count. */
#define BLOB_BRIDGE 0x03F00000u
#define BLOB_BYTES 324944u

/* The read contract, quoted from the bus documentation: "Upon receiving
 * a read the core may not immediately provide the read data and has up
 * until the next read strobe to drive bridge_rd_data." The strobe asks
 * and the word is taken before the next strobe goes out.
 *
 * How much later is not published as a number. The same page gives the
 * bus as "a few megabytes per second", which is about a microsecond for
 * a thirty-two bit word: a model of the host, not a rule it stated. */
#define HOST_GAP 74

static long g_underrun_at;

/* The card as installed, with the ROM the user picked already bound.
 * There is no blob here on purpose: the real host streams the state in
 * during Runtime, after Reset Exit, onto a machine that has already
 * booted and started the ROM -- measured off a device, not assumed.
 * The old shape of this bench wrote the blob before the core ran, and
 * every restore it ever tested landed on an idle machine that had
 * declined its ROM; hardware never once takes that path. */
/* A sleep cuts the power to the core, not to the SD card: keep_card is
 * a wake, where the files are where they were and only the host's
 * binding of a data slot to one of them is gone. A plain boot is a
 * device that has never seen this card. */
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
    tb_load_tcm(r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm0,
                r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm1,
                r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm2,
                r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm3,
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

/* 0x00A0 with the request bit, then the query the host polls with. */
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
    /* The ack is combinational, so seeing it says nothing about the
     * core having had a clock edge to latch the request on. The
     * bridge's command engine does not let go the instant it is
     * answered either -- it leaves for its done state, which is a few
     * clocks. Hold it the same way. */
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
        /* Not host_read: that helper samples six cycles after the
         * strobe, which suits the file window's held registers. The
         * documented contract gives the core until the NEXT strobe to
         * drive the data, and the serializer uses most of that -- so
         * the blob is taken the way the contract describes, at the end
         * of the gap. */
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

/* The machine's own arrays, reached the way every other bench on this
 * board reaches them: straight out of the model, not through the blob,
 * so a blob that agreed with itself would still be caught. */
#define MEM(f) (dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__##f)

static const std::string DONE = "pocket file ok";

/* Where the save lands, counted from the firmware letting the 6502 go
 * rather than from the bench's clock. The release itself moves by a few
 * thousand steps whenever the firmware's boot path changes, and the run
 * after it is short: about fifteen thousand steps in the program, with
 * its first console byte another three hundred thousand away.
 *
 * Short is not incidental. The save must land before the program's
 * first file operation, because a state taken partway through one comes
 * back to a host that never heard of it -- see the README. The machine
 * holds off a freeze while a single command is outstanding, which is
 * not the same as holding off across a whole operation, and this bench
 * is where that difference was found. */
#define SAVE_MARGIN 15000L

/* --- The picture, either side of a freeze ---
 *
 * pocket_video's two rasters run at the same rate off the same PLL with
 * the reader a fixed few pixels behind the writer, and that offset is
 * the whole of the design: every pixel the machine pushes is popped
 * exactly once, in order, by a reader that never has to guess. A
 * savestate stops the writer mid-frame and starts it again somewhere
 * else, so the offset is the thing to measure across one. Constant is
 * correct. Anything else is the picture sliding, and it slides for the
 * rest of the session.
 */
#define VID(f) (dut->rootp->tb_pocket__DOT__core__DOT__video__DOT__##f)

/* Where the machine's beam is each time the reader starts a frame, in
 * pixels from the beam's own origin. Sampled by stepping. */
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
                return -1; /* it moved: the lock is not a lock */
            seen = at;
        }
        prev_start = start;
    }
    return seen;
}

/* Steps the machine to the save point: reset held while the firmware
 * stages, then SAVE_MARGIN of the program. */
static void step_to_save_point(void)
{
    while (!(int)MEM(resb))
        step();
    for (long i = 0; i < SAVE_MARGIN; i++)
        step();
}

/* The same span, with the beam sampled across it. */
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
                return -1; /* it moved: the lock is not a lock */
            seen = at;
        }
        prev_start = start;
    }
    return seen;
}


/* What the host actually hands back at a load: the blob inside the
 * file the OS kept, with the OS's own header in front and a thumbnail
 * behind. 596 and 52764 bytes on the device this was measured on; the
 * junk here is patterned so no three consecutive words can pass for
 * magic, version and length. */
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

    /* Far enough in that the machine is real -- the firmware up, the
     * ROM staged and running, the program partway through writing a
     * file -- and nowhere near finished, because a program that has
     * already stopped proves nothing about carrying one across. */
    /* Sampled on the way to the save point rather than around it: where
     * the save lands is chosen to the step, and a bench that stopped to
     * measure would be saving a different machine. */
    long align_pre = vid_offset_to_save();
    /* The program works in silence and prints only when it is finished,
     * so an empty console here is the whole point: what is about to be
     * saved is a machine in the middle of a file. And the 6502 is out
     * of reset, or what is being saved is the firmware still staging
     * and no program at all. */
    ASSERT_EQ(0u, g_console.size());
    ASSERT_TRUE((int)MEM(resb));
    /* The offset the freeze has to preserve, on the record. */
    ASSERT_GE(align_pre, 0L);
    /* The terminal's raster window, which is fabric no blob carries and
     * which only vid_restore() can put back. Nonzero because the boot
     * programmed the console into it. */
    uint32_t prog_pre = (uint32_t)MEM(vid_mode0__DOT__prog_shadow);
    ASSERT_NE(0u, prog_pre);
    /* Marks in the memories the program itself will not touch, so that
     * every window the engine reads through is checked and not only the
     * ones the program happens to use. */
    for (uint32_t i = 0; i < 16; i++)
    {
        MEM(xram__DOT__mem0)[i] = (uint8_t)(0xA0 + i);
        MEM(xram__DOT__mem1)[i] = (uint8_t)(0xB0 + i);
        MEM(xram__DOT__mem2)[i] = (uint8_t)(0xC0 + i);
        MEM(xram__DOT__mem3)[i] = (uint8_t)(0xD0 + i);
        MEM(vid_mode0__DOT__cell0)[i] = (uint8_t)(0x10 + i);
        MEM(vid_mode0__DOT__cell1)[i] = (uint8_t)(0x20 + i);
        MEM(vid_prog__DOT__fill_e)[i] = 0x80005000u + i;
        MEM(vid_prog__DOT__spr_c)[i] = 0x99000000u + i;
    }

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    /* The blob must agree with its own trailer before it goes anywhere:
     * a mismatch here is the create mis-serving, which the load would
     * otherwise report as a refusal and leave ambiguous. */
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
    /* Served whole, and none of it late. */
    if (g_underrun_at)
        fprintf(stderr, "underrun at byte %ld, word %ld\n",
                g_underrun_at, g_underrun_at / 4);
    ASSERT_FALSE((int)dut->tb_pocket_savestate_start_err);

    /* A save is not a sleep: the OS takes a state and lets the machine
     * carry on, and on the device it does that every time the user
     * asks for one. The session that was stopped whole must therefore
     * finish its own program before anything is restored anywhere --
     * the machine got its clock back, and nothing about it is
     * different for having been read. */
    for (long i = 0; i < 20000000L && g_console.find(DONE) == std::string::npos;
         i++)
    {
        step();
        if ((i % 4000000L) == 3999999L)
            fprintf(stderr, "after save t=%ldM pc=%04x resb=%d eng=%d "
                            "con=%zu rv=%zu\n",
                    i / 1000000L, (unsigned)MEM(w65c02__DOT__pc), (int)MEM(resb),
                    (int)MEM(engine__DOT__state), g_console.size(),
                    g_rv.size());
    }
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
    /* The create stopped the machine mid-frame and gave it back. The
     * reader stood down and lined up again on the frame's first pixel,
     * so the offset is the one it always was. */
    ASSERT_EQ(align_pre, vid_offset(3L * 525 * 1600));

    teardown();

    /* The wake, the way the device actually does it: a new machine
     * with a cold store boots NORMALLY -- slots streamed, Reset Exit,
     * the firmware up and the ROM staged and running -- and only then,
     * during Runtime, does the host write the state in and ask for the
     * load. What it writes is the file it kept, wrapper and all: the
     * OS header in front of the blob and the thumbnail behind it, at
     * the sizes measured off a real device. The engine finds its magic
     * inside. */
    boot(rom);
    /* Nothing of the old machine crossed by itself. */
    ASSERT_NE(0xA0u, (uint32_t)MEM(xram__DOT__mem0)[0]);
    /* This device is mid-run of its own session -- the same spot the
     * save was taken at, which on hardware is wherever the OS's wake
     * flow happens to land. */
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
        ASSERT_EQ(0x10u + i, (uint32_t)MEM(vid_mode0__DOT__cell0)[i]);
        ASSERT_EQ(0x20u + i, (uint32_t)MEM(vid_mode0__DOT__cell1)[i]);
        ASSERT_EQ(0x80005000u + i, (uint32_t)MEM(vid_prog__DOT__fill_e)[i]);
        ASSERT_EQ(0x99000000u + i, (uint32_t)MEM(vid_prog__DOT__spr_c)[i]);
    }

    /* The whole claim, in one line of console. The 6502 program was
     * halfway through a file when the power went; this device never
     * staged it, never ran a line of it, and it finishes anyway. Its
     * code and its stack are in the SRAM the blob carried, its place in
     * the code is in the registers the engine jammed back, and the
     * syscall it was inside belongs to a soft CPU that resumed at the
     * instruction it was stopped in front of rather than booting. */
    for (long i = 0; i < 20000000L && g_console.find(DONE) == std::string::npos;
         i++)
    {
        step();
        if ((i % 2000000L) == 1999999L)
            fprintf(stderr, "t=%ldM pc=%04x resb=%d con=%zu rv=%zu\n",
                    i / 1000000L, (unsigned)MEM(w65c02__DOT__pc),
                    (int)MEM(resb), g_console.size(), g_rv.size());
    }
    if (g_console.find(DONE) == std::string::npos)
        fprintf(stderr,
                "6502 pc %04x resb=%d running=%d engine=%d console=[%s] rv=[%s]\n",
                (unsigned)MEM(w65c02__DOT__pc), (int)MEM(resb),
                (int)dut->rootp->tb_pocket__DOT__mach_clk_en, (int)MEM(engine__DOT__state),
                g_console.c_str(), g_rv.c_str());
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
    /* And across the wake. This raster belongs to a machine that was
     * power-cycled and reconfigured in between; it lines up with the
     * beam the blob brought back, at the same offset as the machine
     * that made the blob. */
    ASSERT_EQ(align_pre, vid_offset(3L * 525 * 1600));
    /* The window the terminal draws in is back. Nothing in the blob
     * carries it and nothing in the fabric remembers it, so this is
     * the firmware having replayed its own shadow. */
    ASSERT_EQ(prog_pre, (uint32_t)MEM(vid_mode0__DOT__prog_shadow));
    teardown();
}

/* The registers the raster is made of are fabric, and a wake brings
 * them back at their power-on values: console, 480 lines, no terminal
 * window. Nothing in the blob carries them -- it carries the scanline
 * table they describe, and the firmware's own shadows of all three --
 * so this is the firmware replaying what it kept.
 *
 * The canvas is the one that matters, and it needs a program that
 * chose one. On the console ROM a wake boot happens to set the same
 * canvas it went to sleep with, and a bench that used it would pass
 * with the whole restore path deleted.
 */
/* The card is the one thing a savestate cannot carry and cannot rebuild
 * from itself. A data slot's binding to a file belongs to the session
 * the wake ended, so a program holding a file open goes to sleep with a
 * descriptor whose other half no longer exists, and the firmware has to
 * put that half back before the next read.
 *
 * The claim is the stream: what the machine printed before the sleep
 * and what it printed after, laid end to end, are the file exactly once
 * -- no byte lost at the seam and none read twice.
 */
UTEST(psleep, a_file_open_across_the_sleep_is_still_open)
{
    UTEST_SKIP("the firmware does not rebind a descriptor's data slot on wake");

    std::vector<uint8_t> rom = read_file(STREAM_ROM);
    ASSERT_GT(rom.size(), 0u);

    /* Something long enough to be caught in the middle of and easy to
     * read in a diff when it is not. */
    std::vector<uint8_t> payload;
    for (int i = 0; i < 512; i++)
        payload.push_back((uint8_t)('a' + (i % 26)));
    std::string want((const char *)payload.data(), payload.size());

    boot(rom);
    g_files["/Saves/rp6502/common/M.DAT"] = payload;

    /* Partway through the stream: the file is open, some of it has been
     * read, and most of it has not. */
    for (long i = 0; i < 20000000L && g_console.size() < 64; i++)
        step();
    ASSERT_GE(g_console.size(), 64u);
    ASSERT_LT(g_console.size(), want.size());
    std::string before = g_console;

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    /* The wake, on a program of its own, so that everything the console
     * says after the restore is the restored program's. The card is as
     * it was; the bindings are not. */
    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot_into(other, true);
    step_to_save_point();

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    /* The blob has started arriving, so this device's own cold-booted
     * program is stopped: everything it does from here is about to be
     * replaced, and the files it touches on the way are not. */
    /* Within a few passes of the first bridge write into the window,
     * not instantly: the firmware asks once per loop. */
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
    ASSERT_STREQ((before + want.substr(before.size()) + "stream ok\r\n").c_str(),
                 (before + g_console).c_str());
    teardown();
}

/* The other half of the drive: a sleep that lands INSIDE a file
 * operation rather than between two. msc_std_write starts a bridge
 * command, sets msc_busy, and returns to the task loop until it
 * retires, so a blob can carry msc_busy true and a half-issued command
 * whose other end no longer exists. Waiting for the first write puts
 * the freeze in that window.
 */
/* The Memories menu's other half: a state loaded back into a machine
 * that was never power-cycled. Nothing was reconfigured, so the host
 * still has every slot bound to the file it had -- and the firmware
 * asks rather than assuming, so it costs a Get File per open
 * descriptor and no Open File at all.
 *
 * This is the case that says why it asks. Reopening regardless would
 * be eight round trips against a host that had nothing to fix.
 */
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

    /* Same machine, still running, still holding the file open. */
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
    /* It asked, and had nothing to put back. */
    ASSERT_GT(g_getfiles, getfiles_before);
    ASSERT_EQ(opens_before, g_opens);
    teardown();
}

UTEST(psleep, a_sleep_inside_a_file_operation_still_finishes_it)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);

    /* Far enough that the program is in the drive rather than in front
     * of it. */
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
    /* Far enough in that the program has chosen its canvas and then
     * programmed its scanlines: the save point is only where the 6502
     * is let go, and the two happen in that order. */
    for (long i = 0; i < 8000000L
                     && !(uint32_t)MEM(vid_prog__DOT__canvas_shadow);
         i++)
        step();
    for (long i = 0; i < 2000000L; i++)
        step();

    /* 320x240 and a window inside it, neither of which a fresh boot
     * would choose. */
    uint32_t canvas_pre = (uint32_t)MEM(vid_prog__DOT__canvas_shadow);
    uint32_t prog_pre = (uint32_t)MEM(vid_mode0__DOT__prog_shadow);
    uint32_t vsync_pre = (uint32_t)MEM(vid_prog__DOT__vsync_shadow);
    /* 320x240 with 240 programmed lines: a fresh boot chooses console
     * and 480, so all three readings below can tell the two apart. */
    ASSERT_EQ(1u, canvas_pre);
    ASSERT_EQ(240u, vsync_pre);
    ASSERT_NE(0u, prog_pre);

    /* The microsecond counter, which is in the blob for the same reason
     * the canvas is out of it: the firmware's every deadline is an
     * absolute reading of this, held in the TCM the blob does carry. */
    uint64_t mtime_pre = (uint64_t)MEM(rv__DOT__mtime_us);

    std::vector<uint8_t> blob;
    ASSERT_TRUE(create_state(blob));
    teardown();

    /* The wake. It comes up on a console program, because the point is
     * to have a machine whose raster registers are demonstrably not the
     * sleeping one's: this bench boots the wake normally, the way the
     * device does, so a wake that ran the same program would set the
     * same canvas for itself and prove nothing. */
    std::vector<uint8_t> other = read_file(FILE_ROM);
    ASSERT_GT(other.size(), 0u);
    boot(other);
    step_to_save_point();
    ASSERT_EQ(0u, (uint32_t)MEM(vid_prog__DOT__canvas_shadow));
    ASSERT_EQ(480u, (uint32_t)MEM(vid_prog__DOT__vsync_shadow));

    std::vector<uint8_t> file = wrap_blob(blob, 596, 52764);
    host_put_bytes(BLOB_BRIDGE, file.data(), file.size());
    ASSERT_TRUE(load_state());
    /* Up to the release and not a step past it. The fixups run while
     * the 6502 is still held, and clearing the bit is what lets it go,
     * so the engine reaching idle is the last moment at which these
     * three readings can only be the firmware's doing. A program left
     * to run would set its own canvas again within a frame or two and
     * the bench would pass with the whole path deleted. */
    long guard = 0;
    while ((int)MEM(engine__DOT__state) != 0 && guard++ < 20000000L)
        step();
    ASSERT_LT(guard, 20000000L);

    /* The counter reads the sleeping machine's uptime and not this
     * one's. Both are tens of milliseconds here, so the claim is the
     * bound: it is the value the blob carried plus the length of the
     * create, and nowhere near what a wake counts for itself between
     * power-on and the load. Left to restart out of the bitstream it
     * would be the latter, and every deadline the blob brought would
     * sit that far in the future of a machine that meant them now. */
    uint64_t mtime_post = (uint64_t)MEM(rv__DOT__mtime_us);
    ASSERT_GE(mtime_post, mtime_pre);
    ASSERT_LT(mtime_post, mtime_pre + 20000u);

    ASSERT_EQ(canvas_pre, (uint32_t)MEM(vid_prog__DOT__canvas_shadow));
    ASSERT_EQ(prog_pre, (uint32_t)MEM(vid_mode0__DOT__prog_shadow));
    ASSERT_EQ(vsync_pre, (uint32_t)MEM(vid_prog__DOT__vsync_shadow));
    teardown();
}

UTEST(psleep, a_file_with_no_blob_in_it_is_refused_and_the_session_lives)
{
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom);
    step_to_save_point();
    ASSERT_TRUE((int)MEM(resb));

    /* Control: with no savestate at all this session prints DONE well
     * inside the budget below. */
    /* A whole window of junk: nothing in it scans as magic, so the
     * engine gives up at its cap, writes nothing, and says so. The
     * session it interrupted -- stopped whole, resumed whole -- then
     * finishes its own program as if nothing happened, which is also
     * the firmware declining to run restore fixups on a machine that
     * was never restored. */
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
                    i / 1000000L, (unsigned)MEM(w65c02__DOT__pc), (int)MEM(resb),
                    (int)MEM(engine__DOT__state), g_console.size(),
                    (int)dut->rootp->tb_pocket__DOT__mach_clk_en, g_rv.size());
    }
    ASSERT_TRUE(g_console.find(DONE) != std::string::npos);
    teardown();
}
