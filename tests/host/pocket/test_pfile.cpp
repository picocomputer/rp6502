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

/* Each data slot request line from pocket_file is high for one clk_74a
 * cycle. The handlers clock the DUT through tick(), so tick() latches
 * each rising edge, and a request raised while a handler runs is served
 * by the next step(). */
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
        /* mf_datatable registers both the address and the output of
         * port A on clk_74a, so datatable_q holds the word for an address
         * two clk_74a edges after the address is presented. */
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
    /* io_bridge_peripheral.v takes the data for a read only after
     * bridge_addr has moved on to the next word, so the address is moved
     * here before bridge_rd_data is sampled. */
    dut->bridge_addr = addr + 4;
    for (int k = 0; k < 6; k++)
        a_edge();
    return dut->tb_pocket_bridge_rd_data;
}

/* Byte 0 of each word is in bits 31:24, because core_top.sv holds
 * bridge_endian_little low. */
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
    /* The real host does not resolve relative names, so a name that does
     * not start with '/' opens no file. */
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
    /* The host reads the path in the struct as a byte stream but reads
     * each integer as a whole bridge word, so each integer here is put
     * back together into the word that host_get_bytes split. */
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
            target_answer(3); /* file not found */
            return;
        }
        /* The host creates a file only when both the create bit and the
         * resize bit are set. With the create bit alone, or into a folder
         * that does not exist, Open File returns result 1, created and
         * opened, and no file is made. */
        if (!(flags & 2) || !g_dirs.count(parent))
        {
            target_answer(1);
            return;
        }
        it = g_files.emplace(key, std::vector<uint8_t>()).first;
        created = true;
    }
    if (flags & 2)
        it->second.resize(size, 0);
    g_bound[slot] = key;
    dt_set(slot, (uint32_t)it->second.size());
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

static void do_flush()
{
    dut->target_dataslot_done = 0;
    g_flushes++;
    for (int k = 0; k < 4; k++)
        a_edge();
    target_done();
}

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
    /* pocket_file copies wrote_q into wrote_flag when its completion
     * toggle, ret_t, crosses into clk_sys, a few clk_sys cycles after
     * target_dataslot_done rises. */
    for (int k = 0; k < 64; k++)
        a_edge();
    g_getfile_seen++;
    if (dut->rootp->tb_pocket__DOT__core__DOT__file__DOT__wrote_flag)
        g_getfile_wrote++;
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
    g_bound[0] = "/Assets/rp6502/common/pfile.rp6502";
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
    /* pocket_sdram takes no write until its 200 us power-up wait is
     * over, and the bridge's write FIFO holds eight entries, so a load
     * started before tb_pocket_ready overflows the FIFO. */
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
    /* The UTC offset is not zero because one of fstest's checks fails
     * when local time and UTC agree. */
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
    /* The ROM opens its file once for writing and once for reading, and
     * it closes each descriptor without a sync. fs_std_close flushes only
     * a descriptor opened for writing, so exactly one flush is sent. */
    ASSERT_EQ(g_flushes, 1);
    ASSERT_EQ(g_getfiles, 1);

    teardown();
}

static void run_fstest(int *utest_result)
{
    /* The ROM prints BAD on every run and lists the indices of any
     * failed checks after it on the same line, so the loop runs to the
     * end of that line. */
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

    /* The ROM prints its counts in hex, so PASS 38/38 is all 56 checks,
     * which is the line passed() in fstest_rom_gen.py returns. */
    if (g_console.find("PASS 38/38") == std::string::npos)
        fprintf(stderr, "console: [%s]\n", g_console.c_str());
    ASSERT_TRUE(g_console.find("PASS 38/38") != std::string::npos);
}

UTEST(pfile, the_whole_drive_conforms)
{
    std::vector<uint8_t> rom = read_file(FSTEST_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom, false);
    run_fstest(utest_result);
    teardown();
}

UTEST(pfile, a_card_without_the_drives_folder_fails_promptly)
{
    std::vector<uint8_t> rom = read_file(FSTEST_ROM);
    ASSERT_GT(rom.size(), 0u);
    boot(rom, true);

    for (long i = 0; i < 60000000L && g_console.find("PASS") == std::string::npos;
         i++)
        step();

    if (g_console.find("PASS") == std::string::npos)
        fprintf(stderr, "console: [%s]\n", g_console.c_str());
    ASSERT_TRUE(g_console.find("PASS") != std::string::npos);
    size_t made = 0;
    for (std::map<std::string, std::vector<uint8_t>>::const_iterator it
             = g_files.begin();
         it != g_files.end(); ++it)
        if (it->first != g_bound[0])
            made++;
    ASSERT_EQ(made, (size_t)0);
    teardown();
}

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
    ASSERT_TRUE(g_console.find(g_bound[0]) != std::string::npos);
    teardown();
}

UTEST(pfile, every_get_file_is_seen_to_be_answered)
{
    std::vector<uint8_t> rom = read_file(ARGV_ROM);
    ASSERT_GT(rom.size(), 0u);
    g_getfile_seen = 0;
    g_getfile_wrote = 0;
    boot(rom, false);

    for (long i = 0; i < 30000000L && g_getfile_seen < 1; i++)
        step();

    if (g_getfile_wrote != g_getfile_seen)
        fprintf(stderr, "seen=%d wrote=%d console=[%s]\n", g_getfile_seen,
                g_getfile_wrote, g_console.c_str());
    ASSERT_GE(g_getfile_seen, 1);
    ASSERT_EQ(g_getfile_wrote, g_getfile_seen);
    teardown();
}

UTEST(pfile, a_read_is_not_answered_by_someone_elses_command)
{
    std::vector<uint8_t> rom = read_file(SLEEPFILE_ROM);
    boot(rom, false);
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
