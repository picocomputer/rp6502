/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Reproduction bench for the hardware symptom: video damaged after a
 * manual savestate SAVE (no writes) and after a RESTORE.
 *
 * The machine boots and renders the console. Mid-frame, the bench plays
 * the host taking a manual savestate: 0x00A0, wait ready, read the whole
 * blob (which is also what ends the save), and let the machine resume.
 * Then the same blob is written back through the bridge and loaded, the
 * way the Memories menu restores without a power cycle.
 *
 * What is measured, continuously, is the alignment between the two
 * rasters either side of pocket_video's FIFO: the machine's beam
 * (vid_h/vid_v, on the clock that stops) and the scaler-side reader
 * (x/y, on clk_vid, which never stops and never resynchronizes after
 * its first frame pulse). At every reader frame start the writer's beam
 * position is snapshotted; in a healthy run that offset is a constant.
 * FIFO overflow (writer pushes while full: pixel dropped) and underflow
 * (reader pops while empty: stale pixel shown) are counted per frame,
 * and every displayed row is hashed so a vertical shift of the picture
 * can be measured directly.
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "tb_stage.h"
#include "tb_tcm.h"

#include <cstdint>
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
static long g_tick;

#define RT (dut->rootp)
#define M(f) (RT->tb_pocket__DOT__core__DOT__machine__DOT__##f)
#define VID(f) (RT->tb_pocket__DOT__core__DOT__video__DOT__##f)

/* --- Raster alignment and picture observation --- */

struct FrameRec
{
    long start_tick;
    uint16_t wv, wh;   /* writer beam at reader frame start */
    bool mach_on;      /* machine clock enabled at frame start and end */
    long air, drop;    /* fifo pops-while-empty / pushes-while-full */
    uint32_t nz_total; /* nonblack pixels displayed this frame */
    uint16_t vis_rows; /* rows with any nonblack pixel */
    uint32_t rowhash[480];
    uint32_t rownz[480];
};

static std::vector<FrameRec> g_frames;
static FrameRec g_cur;
static bool g_cur_open = false;
static uint16_t g_px = 1, g_py = 1;

static void frame_finalize(void)
{
    if (!g_cur_open)
        return;
    g_cur.mach_on = g_cur.mach_on && RT->tb_pocket__DOT__mach_clk_en;
    g_cur.vis_rows = 0;
    for (int r = 0; r < 480; r++)
        if (g_cur.rownz[r])
            g_cur.vis_rows++;
    g_frames.push_back(g_cur);
    g_cur_open = false;
}

static void frame_start(void)
{
    frame_finalize();
    memset(&g_cur, 0, sizeof g_cur);
    g_cur.start_tick = g_tick;
    g_cur.wv = M(vid_v);
    g_cur.wh = M(vid_h);
    g_cur.mach_on = RT->tb_pocket__DOT__mach_clk_en;
    g_cur_open = true;
}

static void tick(void)
{
    long next = a_next < s_next ? a_next : s_next;
    bool sedge = next == s_next;
    bool aedge = next == a_next;
    bool vidrise = sedge && (g_sys & 1) == 0;
    g_tick++;

    /* The model's own overflow/underflow $error aborts the sim on the
     * first post-resume frame -- which is itself the reproduction. To
     * measure the steady state past it, hold its arming flop low; the
     * bench counts the same conditions itself below. */
    VID(checked) = 0;

    /* Pre-edge state: what this rising edge will act on. */
    bool pre_full = VID(fifo__DOT__wptr_gray)
        == (uint8_t)((~VID(fifo__DOT__rptr_gray_w2) & 0x18)
                     | (VID(fifo__DOT__rptr_gray_w2) & 0x07));
    bool pre_empty = VID(fifo__DOT__rptr_gray) == VID(fifo__DOT__wptr_gray_r2);
    bool pre_wde = RT->tb_pocket__DOT__core__DOT__vid_de;
    bool pre_desel = VID(de_sel);

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
        dut->datatable_q = dt_pipe[1];
        dt_pipe[1] = dt_pipe[0];
        dt_pipe[0] = g_dt[dut->tb_pocket_dt_addr & 63];
        dut->clk_74a = 1;
    }
    dut->eval();
    if (sedge && dut->tb_pocket_tx_valid)
        g_console += (char)dut->tb_pocket_tx_data;

    if (sedge && g_cur_open && pre_wde && pre_full)
        g_cur.drop++;
    if (vidrise)
    {
        if (g_cur_open && pre_desel && pre_empty)
            g_cur.air++;
        uint16_t x = VID(x), y = VID(y);
        if (x == 0 && y == 0 && !(g_px == 0 && g_py == 0) && VID(running))
            frame_start();
        g_px = x;
        g_py = y;
        /* The output registers pair de and rgb one edge behind de_sel;
         * post-edge y is exact for every de pixel (rows change only in
         * the porch). */
        if (g_cur_open && dut->tb_pocket_de && y < 480)
        {
            uint32_t rgb = dut->tb_pocket_rgb;
            g_cur.rowhash[y] = (g_cur.rowhash[y] * 16777619u) ^ rgb;
            if (rgb != 0)
            {
                g_cur.rownz[y]++;
                g_cur.nz_total++;
            }
        }
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

static void a_edge(void)
{
    long t = a_next;
    while (a_next == t)
        tick();
}

/* --- The bridge, from the host's end (as test_psleep plays it) --- */

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

/* --- Target commands, minimal (identical protocol to test_psleep) --- */

static void target_done(void)
{
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = 0;
    for (int k = 0; k < 4; k++)
        a_edge();
}

static void do_openfile(void)
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint8_t param[264];
    host_get_bytes(dut->tb_pocket_param_struct, param, sizeof param);
    std::string name((const char *)param);
    if (name.empty() || name[0] != '/')
    {
        dut->target_dataslot_done = 1;
        dut->target_dataslot_err = 4;
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
    bool created = false;
    auto it = g_files.find(key);
    if (it == g_files.end())
    {
        if (!(flags & 1))
        {
            dut->target_dataslot_done = 1;
            dut->target_dataslot_err = 3;
            for (int k = 0; k < 4; k++)
                a_edge();
            return;
        }
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
    dut->target_dataslot_done = 1;
    dut->target_dataslot_err = created ? 1 : 0;
    for (int k = 0; k < 4; k++)
        a_edge();
}

static void do_slotread(void)
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t off = dut->tb_pocket_ds_slotoffset;
    uint32_t len = dut->tb_pocket_ds_length;
    uint32_t at = dut->tb_pocket_ds_bridgeaddr;
    std::vector<uint8_t> &f = g_files[g_bound[slot]];
    std::vector<uint8_t> chunk(len, 0);
    for (uint32_t i = 0; i < len && off + i < f.size(); i++)
        chunk[i] = f[off + i];
    host_put_bytes(at, chunk.data(), chunk.size());
    target_done();
}

static void do_slotwrite(void)
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t off = dut->tb_pocket_ds_slotoffset;
    uint32_t len = dut->tb_pocket_ds_length;
    uint32_t at = dut->tb_pocket_ds_bridgeaddr;
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

static void do_flush(void)
{
    dut->target_dataslot_done = 0;
    for (int k = 0; k < 4; k++)
        a_edge();
    target_done();
}

static void do_getfile(void)
{
    dut->target_dataslot_done = 0;
    uint32_t slot = dut->tb_pocket_ds_id;
    uint32_t at = dut->tb_pocket_resp_struct;
    const std::string &name = g_bound[slot];
    std::vector<uint8_t> resp(256, 0);
    for (size_t i = 0; i < name.size() && i + 1 < resp.size(); i++)
        resp[i] = (uint8_t)name[i];
    host_put_bytes(at, resp.data(), resp.size());
    target_done();
}

static void step(void)
{
    static int prev_r, prev_w, prev_o, prev_f, prev_g;
    tick();
    int r = dut->tb_pocket_ds_read, w = dut->tb_pocket_ds_write,
        o = dut->tb_pocket_ds_openfile, f = dut->tb_pocket_ds_flush,
        g = dut->tb_pocket_ds_getfile;
    if (o && !prev_o)
    {
        prev_r = prev_w = prev_o = prev_f = prev_g = 0;
        do_openfile();
        return;
    }
    if (f && !prev_f)
    {
        prev_r = prev_w = prev_o = prev_f = prev_g = 0;
        do_flush();
        return;
    }
    if (g && !prev_g)
    {
        prev_r = prev_w = prev_o = prev_f = prev_g = 0;
        do_getfile();
        return;
    }
    if (r && !prev_r)
    {
        prev_r = prev_w = prev_o = prev_f = prev_g = 0;
        do_slotread();
        return;
    }
    if (w && !prev_w)
    {
        prev_r = prev_w = prev_o = prev_f = prev_g = 0;
        do_slotwrite();
        return;
    }
    prev_r = r;
    prev_w = w;
    prev_o = o;
    prev_f = f;
    prev_g = g;
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

#define BLOB_BRIDGE 0x03F00000u
#define BLOB_BYTES 324944u
#define HOST_GAP 74

/* sst_engine's own map, in words. */
#define W_HDR 16u
#define W_STATE 64u
#define W_REGS 256u
#define W_SRAM 16384u
#define W_XRAM 16384u
#define W_CELLS 15360u
#define W_XPROG 8192u
#define B_CELLS (W_HDR + W_STATE + W_REGS + W_SRAM + W_XRAM)
#define B_XPROG (B_CELLS + W_CELLS)

static void boot(const std::vector<uint8_t> &rom)
{
    dut = new Vtb_pocket;
    a_next = s_next = g_sys = 0;
    g_tick = 0;
    g_console.clear();
    g_files.clear();
    g_dirs = {"/", "/Assets", "/Assets/rp6502", "/Assets/rp6502/common",
              "/Saves", "/Saves/rp6502", "/Saves/rp6502/common"};
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

static long g_underrun_at = 0;

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
    for (int k = 0; k < 4; k++)
        a_edge();
    dut->savestate_start = 0;
    dut->eval();
    if (!acked)
        return false;
    for (long i = 0; i < 80000000L && dut->tb_pocket_savestate_start_busy; i++)
        step();
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
    for (long i = 0; i < 60000000L && dut->tb_pocket_savestate_load_busy; i++)
        step();
    return !dut->tb_pocket_savestate_load_busy
           && dut->tb_pocket_savestate_load_ok
           && !dut->tb_pocket_savestate_load_err;
}

/* --- Analysis helpers --- */

static uint32_t blob_w(const std::vector<uint8_t> &b, uint32_t idx)
{
    return ((uint32_t)b[idx * 4] << 24) | ((uint32_t)b[idx * 4 + 1] << 16)
           | ((uint32_t)b[idx * 4 + 2] << 8) | (uint32_t)b[idx * 4 + 3];
}

static void print_frames(const char *tag, size_t from)
{
    for (size_t i = from; i < g_frames.size(); i++)
    {
        const FrameRec &f = g_frames[i];
        printf("%s frame %zu: tick=%ld mach=%d writer=(v=%u,h=%u) "
               "air=%ld drop=%ld nz=%u vis_rows=%u\n",
               tag, i, f.start_tick, (int)f.mach_on, f.wv, f.wh, f.air,
               f.drop, f.nz_total, f.vis_rows);
    }
    fflush(stdout);
}

/* Rows of `post` matched against rows of `pre` by hash; returns the
 * modal vertical displacement of visible content, and prints the map. */
static void row_shift(const FrameRec &pre, const FrameRec &post)
{
    std::map<int, int> deltas;
    int matched = 0, moved = 0, lost = 0;
    for (int r = 0; r < 480; r++)
    {
        if (!pre.rownz[r])
            continue;
        bool found = false;
        for (int q = 0; q < 480; q++)
        {
            if (post.rownz[q] && post.rowhash[q] == pre.rowhash[r])
            {
                deltas[q - r]++;
                matched++;
                if (q != r)
                    moved++;
                found = true;
                break;
            }
        }
        if (!found)
            lost++;
    }
    printf("  row match: %d matched (%d displaced), %d visible rows of the "
           "reference not found anywhere\n", matched, moved, lost);
    for (auto &d : deltas)
        printf("    displacement %+d rows: %d rows\n", d.first, d.second);
    fflush(stdout);
}

static size_t run_frames(int mach_frames, long cap)
{
    size_t from = g_frames.size();
    for (long i = 0; i < cap; i++)
    {
        step();
        int got = 0;
        for (size_t k = from; k < g_frames.size(); k++)
            if (g_frames[k].mach_on)
                got++;
        if (got >= mach_frames)
            break;
    }
    return from;
}

int main(int argc, char **argv)
{
    Verilated::commandArgs(argc, argv);
    /* The model's own fifo checks $stop on the first post-resume frame,
     * which would end the measurement at the moment it gets interesting. */
    Verilated::threadContextp()->errorLimit(1 << 30);
    std::vector<uint8_t> rom = read_file(FILE_ROM);
    if (rom.empty())
    {
        fprintf(stderr, "no FILE_ROM\n");
        return 1;
    }
    boot(rom);
    printf("booted: tick=%ld console=[%.40s]\n", g_tick, g_console.c_str());

    /* Let the machine render real frames. The firmware's boot console
     * is live well before the 6502 program finishes; require frames
     * with content so a vertical shift is measurable. */
    long cap = 14000000L;
    size_t pre_from = g_frames.size();
    int content_frames = 0;
    for (long i = 0; i < cap && content_frames < 3; i++)
    {
        step();
        content_frames = 0;
        for (size_t k = pre_from; k < g_frames.size(); k++)
            if (g_frames[k].mach_on && g_frames[k].vis_rows > 2)
                content_frames++;
    }
    printf("pre-save: %zu frames total, console=[%.60s]\n",
           g_frames.size(), g_console.c_str());
    print_frames("pre", pre_from);
    if (g_frames.size() < 2)
    {
        fprintf(stderr, "no frames rendered; bench broken\n");
        return 1;
    }
    size_t pre_last = g_frames.size() - 1;

    /* Machine-side arrays, snapshotted for the no-corruption check. */
    std::vector<uint8_t> c0(15360), c1(15360), c2(15360), c3(15360);
    std::vector<uint32_t> fe(2048), sc(2048);
    std::vector<uint16_t> fc(2048);
    std::vector<uint32_t> se(2048);
    for (int i = 0; i < 15360; i++)
    {
        c0[i] = M(vid_mode0__DOT__cell0)[i];
        c1[i] = M(vid_mode0__DOT__cell1)[i];
        c2[i] = M(vid_mode0__DOT__cell2)[i];
        c3[i] = M(vid_mode0__DOT__cell3)[i];
    }
    for (int i = 0; i < 2048; i++)
    {
        fe[i] = M(vid_prog__DOT__fill_e)[i];
        fc[i] = M(vid_prog__DOT__fill_c)[i];
        se[i] = M(vid_prog__DOT__spr_e)[i];
        sc[i] = M(vid_prog__DOT__spr_c)[i];
    }

    /* The save: stop, serve the whole blob, resume. */
    long save_asked = g_tick;
    std::vector<uint8_t> blob;
    if (!create_state(blob))
    {
        fprintf(stderr, "create_state failed\n");
        return 1;
    }
    long save_served = g_tick;
    printf("save: asked at tick %ld, blob served by %ld (%ld ticks stopped-ish)"
           " underrun_at=%ld start_err=%d\n",
           save_asked, save_served, save_served - save_asked, g_underrun_at,
           (int)dut->tb_pocket_savestate_start_err);

    /* Blob words for the render's own memories against the model. */
    {
        int bad_cells = 0, bad_xprog = 0;
        for (uint32_t i = 0; i < W_CELLS; i++)
        {
            uint32_t w = ((uint32_t)c3[i] << 24) | ((uint32_t)c2[i] << 16)
                         | ((uint32_t)c1[i] << 8) | c0[i];
            if (blob_w(blob, B_CELLS + i) != w)
                bad_cells++;
        }
        for (uint32_t e = 0; e < 2048; e++)
        {
            if (blob_w(blob, B_XPROG + e * 4 + 0) != fe[e])
                bad_xprog++;
            if (blob_w(blob, B_XPROG + e * 4 + 1) != (uint32_t)fc[e])
                bad_xprog++;
            uint32_t sew = ((se[e] >> 19) & 1) << 31 | (se[e] & 0x7FFFF);
            if (blob_w(blob, B_XPROG + e * 4 + 2) != sew)
                bad_xprog++;
            if (blob_w(blob, B_XPROG + e * 4 + 3) != sc[e])
                bad_xprog++;
        }
        printf("blob serve check: %d cell words wrong, %d xprog words wrong\n",
               bad_cells, bad_xprog);
    }

    /* Resume, then watch. */
    for (long i = 0; i < 4000000L && !RT->tb_pocket__DOT__mach_clk_en; i++)
        step();
    printf("machine clock back: mach=%d resb=%d tick=%ld\n",
           (int)RT->tb_pocket__DOT__mach_clk_en, (int)M(resb), g_tick);

    {
        int dc = 0, dp = 0;
        for (int i = 0; i < 15360; i++)
            if (c0[i] != M(vid_mode0__DOT__cell0)[i]
                || c1[i] != M(vid_mode0__DOT__cell1)[i]
                || c2[i] != M(vid_mode0__DOT__cell2)[i]
                || c3[i] != M(vid_mode0__DOT__cell3)[i])
                dc++;
        for (int i = 0; i < 2048; i++)
            if (fe[i] != M(vid_prog__DOT__fill_e)[i]
                || fc[i] != M(vid_prog__DOT__fill_c)[i]
                || se[i] != M(vid_prog__DOT__spr_e)[i]
                || sc[i] != M(vid_prog__DOT__spr_c)[i])
                dp++;
        printf("post-save arrays: %d cell words changed, %d xprog entries "
               "changed (0 expected: save must not write)\n", dc, dp);
    }

    size_t post_from = run_frames(4, 16000000L);
    printf("post-save frames:\n");
    print_frames("post", post_from);

    /* Alignment verdict for the save. */
    {
        const FrameRec &a = g_frames[pre_last];
        const FrameRec *b = nullptr;
        for (size_t i = g_frames.size(); i-- > post_from;)
            if (g_frames[i].mach_on)
            {
                b = &g_frames[i];
                break;
            }
        if (b)
        {
            long pa = (long)a.wv * 800 + a.wh;
            long pb = (long)b->wv * 800 + b->wh;
            printf("SAVE alignment: pre writer-at-reader-frame-start=(%u,%u) "
                   "post=(%u,%u) delta=%ld pixels (%ld rows)\n",
                   a.wv, a.wh, b->wv, b->wh, pb - pa, (pb - pa) / 800);
            printf("SAVE fifo: pre air=%ld drop=%ld post air=%ld drop=%ld\n",
                   a.air, a.drop, b->air, b->drop);
            row_shift(a, *b);
        }
    }

    /* The restore, in place, the way the Memories menu does it: the
     * blob back through the bridge, then 0x00A4. */
    printf("writing blob back through the bridge...\n");
    fflush(stdout);
    host_put_bytes(BLOB_BRIDGE, blob.data(), blob.size());
    size_t preload_last = g_frames.size() - 1;
    long load_asked = g_tick;
    bool ok = load_state();
    printf("load: ok=%d err=%d busy=%d (%ld ticks)\n", (int)ok,
           (int)dut->tb_pocket_savestate_load_err,
           (int)dut->tb_pocket_savestate_load_busy, g_tick - load_asked);
    for (long i = 0; i < 8000000L && !RT->tb_pocket__DOT__mach_clk_en; i++)
        step();
    printf("machine clock back after load: mach=%d resb=%d\n",
           (int)RT->tb_pocket__DOT__mach_clk_en, (int)M(resb));

    {
        int dc = 0;
        for (int i = 0; i < 15360; i++)
            if (c0[i] != M(vid_mode0__DOT__cell0)[i]
                || c1[i] != M(vid_mode0__DOT__cell1)[i]
                || c2[i] != M(vid_mode0__DOT__cell2)[i]
                || c3[i] != M(vid_mode0__DOT__cell3)[i])
                dc++;
        printf("post-restore cells vs save-time snapshot: %d words differ\n",
               dc);
    }

    size_t post2_from = run_frames(4, 16000000L);
    printf("post-restore frames:\n");
    print_frames("post2", post2_from);
    {
        const FrameRec &a = g_frames[preload_last];
        const FrameRec *b = nullptr;
        for (size_t i = g_frames.size(); i-- > post2_from;)
            if (g_frames[i].mach_on)
            {
                b = &g_frames[i];
                break;
            }
        if (b)
        {
            long pa = (long)a.wv * 800 + a.wh;
            long pb = (long)b->wv * 800 + b->wh;
            printf("RESTORE alignment: pre=(%u,%u) post=(%u,%u) delta=%ld "
                   "pixels (%ld rows)\n",
                   a.wv, a.wh, b->wv, b->wh, pb - pa, (pb - pa) / 800);
            printf("RESTORE fifo: pre air=%ld drop=%ld post air=%ld drop=%ld\n",
                   a.air, a.drop, b->air, b->drop);
            row_shift(g_frames[pre_last], *b);
        }
    }

    printf("console at end: [%.100s]\n", g_console.c_str());
    dut->final();
    delete dut;
    return 0;
}
