/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The whole Pocket stack: the host streams a real .rp6502 over the
 * bridge into SDRAM, the run gate releases the machine, the firmware
 * loads the ROM through the stalled staging window, and the program
 * paints. The frame the scaler receives — decoded from vs/hs/de at
 * 25.2 MHz and unpacked from RGB888 — must be settled and match the
 * CRC written down beside each case, across every canvas geometry the
 * mapping owns. What the machine paints into that frame is checked in
 * tests/cpu/vga against these same ROMs, on both machines; what is
 * left here is the trip out through the scaler. The reload case
 * runs the host's mid-session order — Reset Enter, a new slot,
 * completion, Exit — with a button held through the whole load: the
 * scaler re-arms on the new machine's first frame and the held press
 * delivers itself the moment the rebooted firmware can hear it.
 *
 * THE HOST MODELLED HERE IS OURS, NOT ANALOGUE'S. The bridge's timing,
 * the order of a mid-session reload, and the data slot handshake were
 * reverse engineered from a real Pocket rather than read out of a
 * specification, because there is not one. Expect errors in it. The
 * frame half of this test is exact, but everything the bench does as
 * the host is a guess we have not been able to disprove.
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "corpus.h"
#include "host/host.h"

#include "tb_asm.h"
#include "tb_rom.h"
#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vtb_pocket *dut;
/* The host's id/size pairs, and the image behind the ROM slot. The host
 * pushes the whole image into the store at boot; the copy here answers
 * the slot reads an exec pull would make. */
static uint32_t g_dt[64];
static uint32_t dt_pipe[2];
static std::vector<uint8_t> g_rom;
static int g_rd_prev, g_rd_hold;

static long a_next, s_next;
static long g_t, g_sys;

/* Get File state, reset per power-on because the model outlives the
 * device it answers for. */
static int g_gf_prev, g_gf_hold;

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
    {
        dut->datatable_q = dt_pipe[1];
        dt_pipe[1] = dt_pipe[0];
        dt_pipe[0] = g_dt[dut->tb_pocket_dt_addr & 63];
        dut->clk_74a = 1;
    }
    dut->eval();
    if (aedge)
    {
        /* argv: the core asks for the ROM slot's name before it releases
         * the 6502, and a command the host never retires costs it the
         * bridge's whole deadline — 134 million clocks, which is not a
         * test, it is a hang. These cases do not check the name, and
         * the staging store answers zeros, which is the empty name a
         * slot with nothing bound would give. The handshake is what has
         * to happen: done falls so the core can prove its command was
         * taken, then rises carrying the answer. */
        int g = dut->tb_pocket_ds_getfile;
        if (g && !g_gf_prev)
        {
            dut->target_dataslot_done = 0;
            g_gf_hold = 4;
        }
        g_gf_prev = g;

        int r = dut->tb_pocket_ds_read;
        if (r && !g_rd_prev)
        {
            uint32_t off = dut->tb_pocket_ds_slotoffset;
            uint32_t br = dut->tb_pocket_ds_bridgeaddr;
            uint32_t len = dut->tb_pocket_ds_length;
            auto &chip = dut->rootp->tb_pocket__DOT__chip__DOT__mem;
            if (dut->tb_pocket_ds_id == 0)
                for (uint32_t i = 0; i < len; i += 2)
                {
                    size_t a = off + i;
                    uint8_t lo = a < g_rom.size() ? g_rom[a] : 0;
                    uint8_t hi = (i + 1 < len && a + 1 < g_rom.size())
                                     ? g_rom[a + 1]
                                     : 0;
                    chip[(br + i) >> 1] = (uint16_t)(lo | (hi << 8));
                }
            dut->target_dataslot_done = 0;
            g_rd_hold = 4;
        }
        g_rd_prev = r;
        if (g_rd_hold && !--g_rd_hold)
            dut->target_dataslot_done = 1;

        if (g_gf_hold && !--g_gf_hold)
            dut->target_dataslot_done = 1;
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
        r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm0,
        r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm1,
        r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm2,
        r->tb_pocket__DOT__core__DOT__machine__DOT__soc__DOT__tcm3, path);
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
        if (dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__resb)
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
            || !r->tb_pocket__DOT__core__DOT__machine__DOT__resb;
        if (ran && stopped && !moved)
            return true;
        moved = false;
    }
    if (getenv("POCKET_DEBUG"))
        fprintf(stderr, "quiet gave up: frames=%ld ran=%d run=%d "
                "resb=%d stop=%d\n", frames, (int)ran,
                (int)dut->rootp->tb_pocket__DOT__core__DOT__run,
                (int)dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__resb,
                (int)dut->rootp
                    ->tb_pocket__DOT__core__DOT__machine__DOT__cpu__DOT__stop_flag);
    return false;
}

/* Capture one full decoded frame off the scaler interface the way the
 * scaler latches it: sample on clk_vid rises, arm at a vs pulse, collect
 * pixels where de stands and skip does not, and read the end-of-line
 * word on the cycle after each de fall — rgb[23:13] carries the scaler
 * slot request there, rgb[2:0] the function code. The frame is over at
 * the next vs; the caller asserts the pixel count, because the count IS
 * the claim now: a canvas-native stream has exactly canvas-many pixels,
 * and a duplicated or shifted one shows up as the wrong total before
 * any color is compared. */
static size_t capture_frame(uint32_t *fb, size_t max_px, int *slot_out)
{
    size_t at = 0;
    bool started = false;
    bool de_q = false;
    int slot = -1;
    for (;;)
    {
        long sys_before = g_sys;
        tick();
        bool vid_rise = g_sys != sys_before && ((sys_before & 1) == 0);
        if (!vid_rise)
            continue;
        if (dut->tb_pocket_vs)
        {
            if (started && at > 0)
                break; /* the frame between two vs pulses */
            at = 0;
            started = true;
            continue;
        }
        if (!started)
            continue;
        /* The end-of-line word rides the FIRST de-low cycle — the spec's
         * "after the DE falling edge" is this sample, not the one after
         * it, and reading one late finds zeros that decode as slot 0. */
        if (de_q && !dut->tb_pocket_de && (dut->tb_pocket_rgb & 0x7) == 0)
            slot = (int)(dut->tb_pocket_rgb >> 13);
        de_q = dut->tb_pocket_de;
        if (dut->tb_pocket_de && !dut->tb_pocket_skip && at < max_px)
            fb[at++] = dut->tb_pocket_rgb;
    }
    if (slot_out)
        *slot_out = slot;
    return at;
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

/* Host slot load, in the host's own order: the next request drops the
 * completion level, the whole image streams over the bridge to the
 * primary slot's address, the table says how long it is, completion
 * returns and stays. The copy kept in g_rom answers any exec pull
 * afterwards. A hot reload from the Core Settings menu was measured
 * doing exactly this sequence again mid-run — no 0x008A — so this one
 * function is boot and reload both. */
static void host_load(const std::vector<uint8_t> &rom)
{
    dut->dataslot_allcomplete = 0;
    g_rom = rom;
    host_stream(rom, TB_STAGE_ROM_BASE);
    memset(g_dt, 0, sizeof g_dt);
    g_dt[0] = 0;
    g_dt[1] = (uint32_t)rom.size();
    /* The drop has to be clocked before the rise, or the settle has no
     * edge to fire on; the stream's clocks are that, and the wait keeps
     * the guarantee even for an empty image. */
    for (int i = 0; i < 40; i++)
        a_edge();
    dut->dataslot_allcomplete = 1;
}

/* Check the settled scaler frame against the CRC in the case. `wait` is
 * false when the caller already ran the machine to quiet — the judgment
 * can only be made once per run, since a finished machine and one that
 * has not started look alike from outside.
 *
 * Set RP6502_BLESS_CRC to have a run print each observed value in the
 * form it is pasted back as. */
static void run_and_check(int *utest_result, const char *name,
                          uint32_t expect, bool wait = true)
{
    int ow = 0, oh = 0;
    ASSERT_TRUE(corpus_size(name, &ow, &oh));

    if (wait)
        ASSERT_TRUE(run_until_quiet());

    /* Canvas-native: the stream is exactly ow x oh pixels. The old
     * compare here re-implemented the duplication in C to expand a
     * 320-wide picture up to 640x480 — the hardware stopped pretending,
     * so the test stops compensating. */
    static uint32_t fb[640 * 480];
    int slot = -1;
    const size_t got = capture_frame(fb, sizeof fb / sizeof *fb, &slot);
    ASSERT_EQ(got, (size_t)ow * (size_t)oh);

    const int want_slot = ow == 640 ? (oh == 480 ? 0 : 1)
                                    : (oh == 240 ? 2 : 3);
    ASSERT_EQ(slot, want_slot);

    uint32_t crc = host_crc32(0, fb, got * sizeof(uint32_t));
    if (getenv("RP6502_BLESS_CRC"))
        fprintf(stderr, "    %-12s 0x%08X\n", name, crc);
    else if (crc != expect)
    {
        fprintf(stderr, "%s: scaler frame crc 0x%08X, expected 0x%08X\n",
                name, crc, expect);
        ASSERT_EQ(crc, expect);
    }
}

static void power_on(int *utest_result)
{
    a_next = 224;
    s_next = 330;
    /* Genuinely powered on, not merely reset: the beam and the video
     * path take no reset now, so a pulse leaves them mid-frame while
     * everything else restarts. */
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vtb_pocket;
    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
    dut->cont1_key = 0;
    dut->dataslot_allcomplete = 0;
    /* Held high between commands, the way the host holds it: the fall is
     * what proves a command was taken. */
    dut->target_dataslot_done = 1;
    g_gf_prev = g_gf_hold = 0;
    /* The read handshake's edge memory too: a case that ended mid-strobe
     * leaves the next one's first read looking like no edge at all. */
    g_rd_prev = g_rd_hold = 0;
    dt_pipe[0] = dt_pipe[1] = 0;
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
     * clocks a case, and the bridge's own path is proven by the ROM,
     * which streams in every case. */
    const std::vector<uint8_t> &fonts = tb_stage_fonts();
    auto &chip = dut->rootp->tb_pocket__DOT__chip__DOT__mem;
    for (size_t i = 0; i + 1 < fonts.size(); i += 2)
        chip[(TB_STAGE_FONT_BASE + i) >> 1] =
            (uint16_t)(fonts[i] | (fonts[i + 1] << 8));

    /* The code page tables ride in on their own slot the same way, and
     * go in the same way here. */
    const std::vector<uint8_t> &oemcp = tb_stage_oemcp();
    for (size_t i = 0; i + 1 < oemcp.size(); i += 2)
        chip[(TB_STAGE_OEMCP_BASE + i) >> 1] =
            (uint16_t)(oemcp[i] | (oemcp[i + 1] << 8));
}

static void run_case(int *utest_result, const char *name, uint32_t expect)
{
    std::vector<uint8_t> rom = read_rom(name);
    ASSERT_TRUE(rom.size() > 0);
    power_on(utest_result);
    host_load(rom);
    dut->reset_n = 1;
    run_and_check(utest_result, name, expect);
}

UTEST(pocket, canvas3_640x480)
{
    run_case(utest_result, "mode3_8bpp", 0x29DB5FC9);

    /* The asset made the whole trip: the slot the host filled, the
     * firmware's read of it, and the store the scanout reads. The face is
     * built from the asset's pieces rather than copied out of one of them,
     * so what is checked is the face — against the number below, the same
     * way every other picture in the suite is checked. What that face has
     * to be is tests/rtl/vga's, against the font.c the asset comes from;
     * this is the trip. */
    auto &f16 = dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__font__DOT__f16;
    static uint32_t face[1024];
    for (size_t i = 0; i < 1024; i++)
        face[i] = f16[i];
    uint32_t fcrc = host_crc32(0, face, sizeof face);
    if (getenv("RP6502_BLESS_CRC"))
        fprintf(stderr, "    %-12s 0x%08X\n", "font16 store", fcrc);
    else
        ASSERT_EQ(fcrc, 0x0A77F72Cu);

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
    run_case(utest_result, "mode3_1bpp", 0x41437F1B);
}

UTEST(pocket, canvas2_320x180_native)
{
    run_case(utest_result, "mode3_4bppr", 0xCF839AEC);
}

UTEST(pocket, canvas4_640x360_native)
{
    run_case(utest_result, "mode3_16bpp", 0xFB9D8EDE);
}

UTEST(pocket, reload_with_a_button_held)
{
    std::vector<uint8_t> first = read_rom("mode3_8bpp");
    ASSERT_TRUE(first.size() > 0);
    power_on(utest_result);
    host_load(first);
    dut->reset_n = 1;
    run_and_check(utest_result, "mode3_8bpp", 0x29DB5FC9);

    /* The host reloads a different slot; a button goes down before
     * the reset and stays down through the whole load. */
    dut->reset_n = 0;
    dut->cont1_key = 1u << 4; /* A */
    std::vector<uint8_t> second = read_rom("mode3_1bpp");
    ASSERT_TRUE(second.size() > 0);
    host_load(second);
    dut->reset_n = 1;

    ASSERT_TRUE(run_until_quiet(nullptr));

    /* The gamepad is state, so a button held across a reboot is simply
     * still held on the other side — there is no delivery to count
     * and no edge to miss while the machine was away. */
    ASSERT_EQ(dut->rootp->tb_pocket__DOT__core__DOT__cont_key_sys[0], 1u << 4);
    dut->cont1_key = 0;
    for (int i = 0; i < 4000; i++)
        tick();
    ASSERT_EQ(dut->rootp->tb_pocket__DOT__core__DOT__cont_key_sys[0], 0u);

    /* And the scaler re-armed on the new machine's frame. */
    run_and_check(utest_result, "mode3_1bpp", 0x41437F1B, false);
}

UTEST(pocket, hot_reload_without_a_reset)
{
    std::vector<uint8_t> first = read_rom("mode3_8bpp");
    ASSERT_TRUE(first.size() > 0);
    power_on(utest_result);
    host_load(first);
    dut->reset_n = 1;
    run_and_check(utest_result, "mode3_8bpp", 0x29DB5FC9);

    /* The Core Settings pick, as the debug log spells it: no Reset
     * Enter, no 0x008A — the request write drops completion, the new
     * image and the table entry arrive, and the closing
     * access-all-complete is the only announcement the machine gets.
     * The firmware has to notice off the posted size alone. */
    std::vector<uint8_t> second = read_rom("mode3_1bpp");
    ASSERT_TRUE(second.size() > 0);
    host_load(second);

    ASSERT_TRUE(run_until_quiet(nullptr));
    run_and_check(utest_result, "mode3_1bpp", 0x41437F1B, false);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vtb_pocket;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}

/* --- The gamepad the dock holds, as the record a program reads --- *
 *
 * Everything else about a controller is proved on the other machine: the
 * script suite plugs gamepads into the emulator and reads the same ten bytes
 * back. Nothing did it here, so the dock's registers reached ria/hid and
 * were never asked what came out -- the one part of this path that is the
 * machine's rather than the transport's.
 *
 * The program is here rather than in a fixture because it is three
 * instructions: map the block and stay up. What it maps is what apf.c
 * fills, so the assertions are on XRAM and not on anything printed. */
static std::vector<uint8_t> gamepad_mapper(uint16_t at)
{
    tb_asm p;
    /* xreg device 0 (the RIA's own devices), channel 0, address 2: the
     * gamepad block, four ten-byte records from `at`. */
    p.push(0);
    p.push(0);
    p.push(2);
    p.pushw(at);
    p.call(0x01);
    uint16_t here = p.here();
    p.jmp(here); /* stay up: the gamepad is reported to a running machine */

    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, TB_ORG, p.b.data(), p.b.size());
    const uint8_t reset[] = {(uint8_t)TB_ORG, (uint8_t)(TB_ORG >> 8)};
    tb_rom_record(rom, 0xFFFC, reset, sizeof reset);
    return rom;
}

/* One byte of XRAM, which the fabric keeps as four byte lanes. */
static uint8_t xram_at(uint16_t a)
{
    auto *r = dut->rootp;
    size_t w = a >> 2;
    switch (a & 3)
    {
    case 0: return r->tb_pocket__DOT__core__DOT__machine__DOT__xram__DOT__mem0[w];
    case 1: return r->tb_pocket__DOT__core__DOT__machine__DOT__xram__DOT__mem1[w];
    case 2: return r->tb_pocket__DOT__core__DOT__machine__DOT__xram__DOT__mem2[w];
    default: return r->tb_pocket__DOT__core__DOT__machine__DOT__xram__DOT__mem3[w];
    }
}

/* The machine keeps running: this one does not stop, so quiet is not the
 * signal and frames are counted instead. */
static void run_frames(int n)
{
    int prev = dut->tb_pocket_vs;
    long budget = (long)n * 900000L;
    while (n > 0 && budget-- > 0)
    {
        tick();
        int vs = dut->tb_pocket_vs;
        if (vs && !prev)
            n--;
        prev = vs;
    }
}

UTEST(pocket, the_gamepad_the_dock_holds_reaches_xram)
{
    static const uint16_t AT = 0xFF00;
    /* The dock's key word: the type nibble on top, then the buttons the
     * Pocket's own face carries. apf.c reads exactly this. */
    static const uint32_t TYPE_PAD = 2u << 28;

    power_on(utest_result);
    host_load(gamepad_mapper(AT));
    dut->reset_n = 1;
    run_frames(10);

    /* Nothing docked: a blank record, and the connected bit clear is the
     * gate a program reads before anything else. */
    ASSERT_EQ((uint32_t)xram_at(AT), 0u);

    /* Docked, with down and A held. */
    dut->cont1_key = TYPE_PAD | (1u << 1) | (1u << 4);
    run_frames(10);
    ASSERT_EQ((uint32_t)(xram_at(AT) & 0x80), 0x80u);   /* connected */
    ASSERT_EQ((uint32_t)(xram_at(AT) & 0x0F), 0x02u);   /* dpad down */
    ASSERT_EQ((uint32_t)(xram_at(AT + 2) & 0x01), 0x01u); /* button0: A */

    /* A release clears only what it named. */
    dut->cont1_key = TYPE_PAD | (1u << 4);
    run_frames(10);
    ASSERT_EQ((uint32_t)(xram_at(AT) & 0x0F), 0x00u);
    ASSERT_EQ((uint32_t)(xram_at(AT + 2) & 0x01), 0x01u);

    /* Player 1 is a record of its own and nothing is in it. */
    ASSERT_EQ((uint32_t)xram_at(AT + 10), 0u);

    /* Undocked blanks the whole record, which is how a program tells a
     * gamepad that let go from one that left. */
    dut->cont1_key = 0;
    run_frames(20);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ((uint32_t)xram_at(AT + i), 0u);
}
