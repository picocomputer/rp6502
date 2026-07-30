/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The whole Pocket stack against the oracle: the host streams a real
 * .rp6502 over the bridge into SDRAM, the run gate releases the
 * machine, the firmware loads the ROM through the stalled staging
 * window, and the program paints. The frame the scaler receives —
 * decoded from vs/hs/de at 25.2 MHz and unpacked from RGB888 — must
 * be settled and pixel-exact against emu_core running the same file,
 * across every canvas geometry the mapping owns. The reload case
 * runs the host's mid-session order — Reset Enter, a new slot,
 * completion, Exit — with a button held through the whole load: the
 * scaler re-arms on the new machine's first frame and the held press
 * delivers itself the moment the rebooted firmware can hear it.
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "oracle.h"
extern "C" {
#include "vga/term/font.h"
}

#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vtb_pocket *dut;

static long a_next, s_next;
static long g_t, g_sys;

/* clk_sys period 330 with clk_vid on every second rise; clk_74a at
 * period 224 — the true PLL family against the bridge clock. */
static void tick()
{
    long next = a_next < s_next ? a_next : s_next;
    g_t = next;
    bool sedge = next == s_next;
    bool aedge = next == a_next;
    if (sedge)
    {
        dut->clk_sys = 1;
        if ((g_sys & 1) == 0)
        {
            /* Both are half clk_sys and rise with it; the soft CPU's
             * comes off the same PLL for exactly that reason. */
            dut->clk_vid = 1;
            dut->clk_rv = 1;
        }
    }
    if (aedge)
        dut->clk_74a = 1;
    dut->eval();
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

/* One clk_74a rising edge. */
static void a_edge()
{
    long t = a_next;
    while (a_next == t)
        tick();
}

static bool load_firmware(const char *path)
{
    auto *r = dut->rootp;
    return tb_load_tcm(
        r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm0,
        r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm1,
        r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm2,
        r->tb_pocket__DOT__core__DOT__machine__DOT__rv__DOT__tcm3, path);
}

/* The run is over when the 6502 has run, stopped, and a frame has
 * passed with no console byte — the same judgment tb_quiet makes for
 * the bare machine, spelled here against the assembled core's own port
 * names. Waiting for the firmware to speak instead, as this did, cost a
 * frame a case: the banner lands after the program is already done. Counts key
 * deliveries on the way when the caller wants them. */
static bool run_until_quiet(long *presses = nullptr)
{
    long frames = 0;
    const long flimit = 20;
    long budget = flimit * 900000L;
    bool moved = false;
    bool ran = false;
    int prev = dut->tb_pocket_vs;
    while (frames < flimit && budget-- > 0)
    {
        long sys_before = g_sys;
        tick();
        if (g_sys == sys_before)
            continue;

        if (dut->tb_pocket_tx_valid || dut->tb_pocket_rv_tx_valid)
            moved = true;
        if (dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__cpu_run)
            ran = true;
        int vs = dut->tb_pocket_vs;
        bool frame_edge = vs && !prev;
        prev = vs;
        if (!frame_edge)
            continue;
        frames++;
        auto *r = dut->rootp;
        bool stopped =
            r->tb_pocket__DOT__core__DOT__machine__DOT__cpu__DOT__stop_flag != 0
            || !r->tb_pocket__DOT__core__DOT__machine__DOT__cpu_run;
        if (ran && stopped && !moved)
            return true;
        moved = false;
    }
    if (getenv("POCKET_DEBUG"))
        fprintf(stderr, "quiet gave up: frames=%ld ran=%d run=%d "
                "cpu_run=%d stop=%d\n", frames, (int)ran,
                (int)dut->rootp->tb_pocket__DOT__core__DOT__run,
                (int)dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__cpu_run,
                (int)dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__cpu__DOT__stop_flag);
    return false;
}

/* Capture one full decoded frame off the scaler interface: sample on
 * clk_vid rises, track the raster from vs, collect de pixels. */
static void capture_frame(uint32_t *fb)
{
    size_t at = 0;
    bool started = false;
    while (at < 640 * 480)
    {
        long sys_before = g_sys;
        tick();
        bool vid_rise = g_sys != sys_before && ((sys_before & 1) == 0);
        if (!vid_rise)
            continue;
        if (dut->tb_pocket_vs)
        {
            at = 0; /* a partial catch restarts at the origin */
            started = true;
            continue;
        }
        if (started && dut->tb_pocket_de)
            fb[at++] = dut->tb_pocket_rgb;
    }
}

/* The oracle's framebuffer is RGBA with the same replication the
 * adapter does; repack it into the scaler's RGB888 lanes. */
static uint32_t scaler_rgb(uint32_t rgba)
{
    uint32_t r = rgba & 0xFF;
    uint32_t g = (rgba >> 8) & 0xFF;
    uint32_t b = (rgba >> 16) & 0xFF;
    return (r << 16) | (g << 8) | b;
}

static std::vector<uint8_t> read_rom(const char *name)
{
    std::vector<uint8_t> rom;
    std::string path = std::string(ROMS_DIR "/") + name + ".rp6502";
    FILE *f = fopen(path.c_str(), "rb");
    if (!f)
        return rom;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        rom.insert(rom.end(), buf, buf + n);
    fclose(f);
    return rom;
}

/* One slot's bytes over the bridge. The slot's own bridge address is
 * where it lands in the SDRAM, which is the whole of the platform's
 * part in this: the font asset is a second slot and needs no more
 * hardware than a different address in data.json. */
static void host_stream(const std::vector<uint8_t> &data, uint32_t base)
{
    for (size_t i = 0; i < data.size(); i += 4)
    {
        uint32_t w = 0;
        for (size_t k = 0; k < 4; k++)
        {
            uint8_t b = i + k < data.size() ? data[i + k] : 0;
            w |= (uint32_t)b << (24 - 8 * k);
        }
        dut->bridge_wr = 1;
        dut->bridge_addr = base + (uint32_t)i;
        dut->bridge_wr_data = w;
        a_edge();
        dut->bridge_wr = 0;
        for (int k = 0; k < 39; k++)
            a_edge();
    }
}

/* Host slot load: the next request drops the completion level, the
 * stream lands, completion returns and stays. Word 1 of the data table
 * is slot 0's size, so only the ROM's is posted. */
static void host_load(const std::vector<uint8_t> &rom)
{
    dut->dataslot_allcomplete = 0;
    dut->datatable_q = (uint32_t)rom.size();
    host_stream(rom, 0);
    dut->dataslot_allcomplete = 1;
}

/* Compare the settled scaler frame against the oracle for the same
 * file. `wait` is false when the caller already ran the machine to
 * quiet — the judgment can only be made once per run, since a
 * finished machine and one that has not started look alike from
 * outside. */
static void run_and_compare(int *utest_result, const char *name,
                            bool wait = true)
{
    ASSERT_TRUE(oracle_restart(
        (std::string(ROMS_DIR "/") + name + ".rp6502").c_str()));
    oracle_run_frames(30);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    const uint32_t *ofb = oracle_framebuffer();

    if (wait)
        ASSERT_TRUE(run_until_quiet());

    static uint32_t fb[2][640 * 480];
    capture_frame(fb[0]);

    int xs = ow == 320 ? 1 : 0;
    int ys = (oh == 240 || oh == 180) ? 1 : 0;
    int yo = (oh == 180 || oh == 360) ? 60 : 0;
    int diffs = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
        {
            uint32_t want = 0;
            if (y >= yo && ((y - yo) >> ys) < oh)
                want = scaler_rgb(
                    ofb[(size_t)((y - yo) >> ys) * (size_t)ow
                        + (size_t)(x >> xs)]);
            if (fb[0][(size_t)y * 640 + x] != want)
            {
                if (getenv("POCKET_DEBUG") && diffs < 20)
                    fprintf(stderr, "diff x=%d y=%d rtl=%06X want=%06X\n",
                            x, y, fb[0][(size_t)y * 640 + x], want);
                diffs++;
            }
        }
    ASSERT_EQ(diffs, 0);
}

static void power_on(int *utest_result)
{
    a_next = 224;
    s_next = 330;
    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
    dut->cont1_key = 0;
    dut->dataslot_allcomplete = 0;
    for (int i = 0; i < 32; i++)
        tick();
    dut->rst_n = 1;
    dut->arst_n = 1;
    long guard = 0;
    while (!dut->tb_pocket_ready && guard++ < 60000)
        tick();
    ASSERT_TRUE(dut->tb_pocket_ready);
    /* The core's own asset, loaded once before the machine runs and
     * never reloaded. Written straight into the chip rather than
     * streamed: sixty kilobytes over the bridge is six hundred thousand
     * clocks a case, and the bridge's own path is already proven by the
     * ROM beside it. One case still streams it, so nothing about the
     * loader goes unexercised. */
    const std::vector<uint8_t> &fonts = tb_stage_fonts();
    auto &chip = dut->rootp->tb_pocket__DOT__chip__DOT__mem;
    for (size_t i = 0; i + 1 < fonts.size(); i += 2)
        chip[(TB_STAGE_FONT_BASE + i) >> 1] =
            (uint16_t)(fonts[i] | (fonts[i + 1] << 8));
}

static void run_case(int *utest_result, const char *name)
{
    std::vector<uint8_t> rom = read_rom(name);
    ASSERT_TRUE(rom.size() > 0);
    power_on(utest_result);
    host_load(rom);
    dut->reset_n = 1;
    run_and_compare(utest_result, name);
}

UTEST(pocket, canvas3_640x480)
{
    run_case(utest_result, "mode3_8bpp");

    /* The asset made the whole trip: host bridge to SDRAM to the
     * firmware's copy to the store the scanout reads. */
    auto &f16 = dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__vid_font__DOT__f16;
    for (size_t i = 0; i < 4096; i += 4)
        ASSERT_EQ(f16[i / 4],
                  (uint32_t)font16[i] | ((uint32_t)font16[i + 1] << 8)
                      | ((uint32_t)font16[i + 2] << 16)
                      | ((uint32_t)font16[i + 3] << 24));

    /* The codec side is alive: LRCK ticks at audio rate. */
    int lrck_flips = 0;
    int lrck_q = dut->tb_pocket_lrck;
    for (int i = 0; i < 200000; i++)
    {
        tick();
        if ((int)dut->tb_pocket_lrck != lrck_q)
        {
            lrck_q = dut->tb_pocket_lrck;
            lrck_flips++;
        }
    }
    ASSERT_GT(lrck_flips, 50);
}

UTEST(pocket, canvas1_320x240)
{
    run_case(utest_result, "mode3_1bpp");
}

UTEST(pocket, canvas2_320x180_letterbox)
{
    run_case(utest_result, "mode3_4bppr");
}

UTEST(pocket, canvas4_640x360_letterbox)
{
    run_case(utest_result, "mode3_16bpp");
}

UTEST(pocket, reload_with_a_button_held)
{
    std::vector<uint8_t> first = read_rom("mode3_8bpp");
    ASSERT_TRUE(first.size() > 0);
    power_on(utest_result);
    host_load(first);
    dut->reset_n = 1;
    run_and_compare(utest_result, "mode3_8bpp");

    /* The host reloads a different slot; a button goes down before
     * the reset and stays down through the whole load. */
    dut->reset_n = 0;
    dut->cont1_key = 1u << 4; /* A */
    std::vector<uint8_t> second = read_rom("mode3_1bpp");
    ASSERT_TRUE(second.size() > 0);
    host_load(second);
    dut->reset_n = 1;

    ASSERT_TRUE(run_until_quiet(nullptr));

    /* The pad is state, so a button held across a reboot is simply
     * still held on the other side — there is no delivery to count
     * and no edge to miss while the machine was away. */
    ASSERT_EQ(dut->rootp->tb_pocket__DOT__core__DOT__pad_key, 1u << 4);
    dut->cont1_key = 0;
    for (int i = 0; i < 4000; i++)
        tick();
    ASSERT_EQ(dut->rootp->tb_pocket__DOT__core__DOT__pad_key, 0u);

    /* And the scaler re-armed on the new machine's frame. */
    run_and_compare(utest_result, "mode3_1bpp", false);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vtb_pocket;
    oracle_init();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
