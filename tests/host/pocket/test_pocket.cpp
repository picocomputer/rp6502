/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vtb_pocket.h"
#include "Vtb_pocket___024root.h"

#include "corpus.h"
#include "host/host.h"
#include "pocket_version.h"

#include "tb_asm.h"
#include "tb_rom.h"
#include "tb_snap.h"
#include "tb_stage.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vtb_pocket *dut;

/* The bench's side of a run: its clocks, what the host has loaded, and the
 * file commands it is answering. power_on starts from Bench{}, and a
 * snapshot keeps the model and a copy of this, which io() lists field by
 * field. */
struct Bench
{
    long a_next = 224, s_next = 330;
    long t = 0, sys = 0;
    /* XRAM's port clock rises 124 and 289 units after each clk_sys edge,
     * which is 7.4 and 17.4 ns, and clk_ph is low across the first and high
     * across the second. */
    long a2_next = 124;
    int a2_ph = 0;
    uint32_t dt[64] = {};
    uint32_t dt_pipe[2] = {};
    std::vector<uint8_t> rom;
    int rd_prev = 0, rd_hold = 0;
    int gf_prev = 0, gf_hold = 0;

    template <typename S> void io(S &s)
    {
        tb_snap_io(s, a_next);
        tb_snap_io(s, s_next);
        tb_snap_io(s, t);
        tb_snap_io(s, sys);
        tb_snap_io(s, a2_next);
        tb_snap_io(s, a2_ph);
        tb_snap_io(s, dt);
        tb_snap_io(s, dt_pipe);
        tb_snap_io(s, rom);
        tb_snap_io(s, rd_prev);
        tb_snap_io(s, rd_hold);
        tb_snap_io(s, gf_prev);
        tb_snap_io(s, gf_hold);
    }
};
static Bench tb;

/* The periods 330 and 224 give clk_sys and clk_74a the ratio of
 * 50.4 MHz to 74.25 MHz. */
static void tick()
{
    long next = tb.a_next < tb.s_next ? tb.a_next : tb.s_next;
    if (tb.a2_next < next)
        next = tb.a2_next;
    tb.t = next;
    bool sedge = next == tb.s_next;
    bool aedge = next == tb.a_next;
    bool a2edge = next == tb.a2_next;
    if (a2edge)
    {
        dut->clk_ph = tb.a2_ph;
        dut->clk_a2 = 1;
    }
    if (sedge)
    {
        dut->clk_sys = 1;
        if ((tb.sys & 1) == 0)
            dut->clk_vid = 1;
    }
    if (aedge)
    {
        dut->datatable_q = tb.dt_pipe[1];
        tb.dt_pipe[1] = tb.dt_pipe[0];
        tb.dt_pipe[0] = tb.dt[dut->tb_pocket_dt_addr & 63];
        dut->clk_74a = 1;
    }
    dut->eval();
    if (aedge)
    {
        /* The firmware issues Get File on the ROM slot before it
         * releases the 6502, and pocket_file waits 2^27 clk_74a cycles
         * for a command that is never completed, so the bench completes
         * it. target_dataslot_done is dropped first because pocket_file
         * waits for it to fall before it waits for it to rise. */
        int g = dut->tb_pocket_ds_getfile;
        if (g && !tb.gf_prev)
        {
            dut->target_dataslot_done = 0;
            tb.gf_hold = 4;
        }
        tb.gf_prev = g;

        int r = dut->tb_pocket_ds_read;
        if (r && !tb.rd_prev)
        {
            uint32_t off = dut->tb_pocket_ds_slotoffset;
            uint32_t br = dut->tb_pocket_ds_bridgeaddr;
            uint32_t len = dut->tb_pocket_ds_length;
            auto &chip = dut->rootp->tb_pocket__DOT__chip__DOT__mem;
            if (dut->tb_pocket_ds_id == 0)
                for (uint32_t i = 0; i < len; i += 2)
                {
                    size_t a = off + i;
                    uint8_t lo = a < tb.rom.size() ? tb.rom[a] : 0;
                    uint8_t hi = (i + 1 < len && a + 1 < tb.rom.size())
                                     ? tb.rom[a + 1]
                                     : 0;
                    chip[(br + i) >> 1] = (uint16_t)(lo | (hi << 8));
                }
            dut->target_dataslot_done = 0;
            tb.rd_hold = 4;
        }
        tb.rd_prev = r;
        if (tb.rd_hold && !--tb.rd_hold)
            dut->target_dataslot_done = 1;

        if (tb.gf_hold && !--tb.gf_hold)
            dut->target_dataslot_done = 1;
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
    if (a2edge)
    {
        dut->clk_a2 = 0;
        tb.a2_next += 165;
        tb.a2_ph ^= 1;
    }
    dut->eval();
}

static void a_edge()
{
    long t = tb.a_next;
    while (tb.a_next == t)
        tick();
}

static bool run_until_quiet(long *presses = nullptr)
{
    long frames = 0;
    const long flimit = 20;
    /* The budget is in clk_sys cycles, 840,000 to a frame, so it stops a run
     * whose video has stopped rather than one that is slow to go quiet, such
     * as a ROM that fills XRAM. It is counted past the clk_sys test because
     * the three clocks tick about four times a clk_sys cycle, and a count of
     * every tick would run out at a quarter of flimit. */
    long budget = flimit * 900000L;
    bool moved = false;
    bool ran = false;
    int prev = dut->tb_pocket_vs;
    while (frames < flimit && budget > 0)
    {
        long sys_before = tb.sys;
        tick();
        if (tb.sys == sys_before)
            continue;
        budget--;

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

static size_t capture_frame(uint32_t *fb, size_t max_px, int *slot_out)
{
    size_t at = 0;
    bool started = false;
    bool de_q = false;
    int slot = -1;
    for (;;)
    {
        long sys_before = tb.sys;
        tick();
        bool vid_rise = tb.sys != sys_before && ((sys_before & 1) == 0);
        if (!vid_rise)
            continue;
        if (dut->tb_pocket_vs)
        {
            if (started && at > 0)
                break;
            at = 0;
            started = true;
            continue;
        }
        if (!started)
            continue;
        /* pocket_video puts the scaler slot in rgb[23:13] only on the
         * first clk_vid cycle with de low and drives zeros after it, so a
         * sample one cycle late decodes as slot 0. */
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

/* A hot reload from the Core Settings menu was measured as a slot
 * request write, the new image and its data table entry, and a second
 * 0x008F, with no 0x008A data slot update. The request write clears
 * dataslot_allcomplete and 0x008F sets it. host_load drives that
 * sequence, so a hot reload is modelled as a second call to it. */
static void host_load(const std::vector<uint8_t> &rom)
{
    dut->dataslot_allcomplete = 0;
    tb.rom = rom;
    host_stream(rom, TB_STAGE_ROM_BASE);
    memset(tb.dt, 0, sizeof tb.dt);
    tb.dt[0] = 0;
    tb.dt[1] = (uint32_t)rom.size();
    /* pocket_bridge posts the slot size on a rising edge of
     * dataslot_allcomplete, so the low level has to be clocked in before
     * it rises. These edges do that even for an empty image, which
     * streams no words. */
    for (int i = 0; i < 40; i++)
        a_edge();
    dut->dataslot_allcomplete = 1;
}

static void run_and_check(int *utest_result, const char *name,
                          uint32_t expect, bool wait = true)
{
    int ow = 0, oh = 0;
    ASSERT_TRUE(corpus_size(name, &ow, &oh));

    if (wait)
        ASSERT_TRUE(run_until_quiet());

    static uint32_t fb[640 * 480];
    int slot = -1;
    const size_t got = capture_frame(fb, sizeof fb / sizeof *fb, &slot);
    ASSERT_EQ(got, (size_t)ow * (size_t)oh);

    /* No two of video.json's scaler_modes share a height, so the height
     * alone names the canvas's entry. */
    int want_slot = -1;
    switch (oh)
    {
    case 480: want_slot = 0; break;
    case 360: want_slot = 1; break;
    case 240: want_slot = 2; break;
    case 180: want_slot = 3; break;
    case 200: want_slot = 4; break;
    case 400: want_slot = 5; break;
    }
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

/* The firmware is written straight into the SDRAM model unless the case
 * streams it itself, because streaming it over the bridge costs about ten
 * clk_74a cycles a byte and the ROM already goes over the bridge in every
 * case. */
static void power_on(int *utest_result, bool firmware = true)
{
    tb = Bench{};
    /* The DUT is rebuilt rather than reset because the timing and
     * pocket_video modules have no reset, so a reset pulse would leave
     * them mid-frame while the rest of the machine restarts. */
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vtb_pocket;
    dut->rst_n = 0;
    dut->arst_n = 0;
    dut->reset_n = 0;
    dut->cont1_key = 0;
    dut->dataslot_allcomplete = 0;
    dut->target_dataslot_done = 1;
    for (int i = 0; i < 32; i++)
        tick();
    dut->rst_n = 1;
    dut->arst_n = 1;
    long guard = 0;
    while (!dut->tb_pocket_ready && guard++ < 60000)
        tick();
    ASSERT_TRUE(dut->tb_pocket_ready);
    auto &chip = dut->rootp->tb_pocket__DOT__chip__DOT__mem;
    if (firmware)
        ASSERT_TRUE(tb_stage_poke_firmware(chip, SW_BIN));
}

/* Powers on with the firmware, loads rom and runs until the machine goes
 * quiet, or starts from the snapshot of that, which the first run writes.
 * Returns false when power_on failed or the machine never went quiet. */
static bool boot_quiet(int *utest_result, const std::vector<uint8_t> &rom)
{
    tb_snap_key k;
    tb_snap_io(k, rom);
    tb_snap_io(k, tb_stage_firmware(SW_BIN));
    k.flush();
    std::string path = tb_snap_path(tb_snap_dir(TEST_SCRATCH, "pocket"), k.h);
    std::vector<uint8_t> v;
    if (tb_snap_read(path, v))
    {
        if (dut)
        {
            dut->final();
            delete dut;
        }
        dut = tb_snap_make<Vtb_pocket>(v, tb);
        return true;
    }
    power_on(utest_result);
    if (*utest_result)
        return false;
    host_load(rom);
    dut->reset_n = 1;
    if (!run_until_quiet())
        return false;
    tb_snap_write(path, tb_snap_take(*dut, tb));
    return true;
}

static void run_case(int *utest_result, const char *name, uint32_t expect)
{
    std::vector<uint8_t> rom = read_rom(name);
    ASSERT_TRUE(rom.size() > 0);
    ASSERT_TRUE(boot_quiet(utest_result, rom));
    run_and_check(utest_result, name, expect, false);
}

UTEST(pocket, canvas3_640x480)
{
    run_case(utest_result, "mode3_8bpp", 0x29DB5FC9);

    auto &f16 = dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__font__DOT__f16;
    static uint32_t face[1024];
    for (size_t i = 0; i < 1024; i++)
        face[i] = f16[i];
    uint32_t fcrc = host_crc32(0, face, sizeof face);
    if (getenv("RP6502_BLESS_CRC"))
        fprintf(stderr, "    %-12s 0x%08X\n", "font16 store", fcrc);
    else
        ASSERT_EQ(fcrc, 0x0A77F72Cu);

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

UTEST(pocket, canvas5_320x200_native)
{
    run_case(utest_result, "mode3_full200", 0x96571946);
}

UTEST(pocket, canvas6_640x400_native)
{
    run_case(utest_result, "mode3_full400", 0xA09E020A);
}

UTEST(pocket, reload_with_a_button_held)
{
    std::vector<uint8_t> first = read_rom("mode3_8bpp");
    ASSERT_TRUE(first.size() > 0);
    ASSERT_TRUE(boot_quiet(utest_result, first));
    run_and_check(utest_result, "mode3_8bpp", 0x29DB5FC9, false);

    dut->reset_n = 0;
    dut->cont1_key = 1u << 4; /* A */
    std::vector<uint8_t> second = read_rom("mode3_1bpp");
    ASSERT_TRUE(second.size() > 0);
    host_load(second);
    dut->reset_n = 1;

    ASSERT_TRUE(run_until_quiet(nullptr));

    ASSERT_EQ(dut->rootp->tb_pocket__DOT__core__DOT__cont_key_sys[0], 1u << 4);
    dut->cont1_key = 0;
    for (int i = 0; i < 4000; i++)
        tick();
    ASSERT_EQ(dut->rootp->tb_pocket__DOT__core__DOT__cont_key_sys[0], 0u);

    run_and_check(utest_result, "mode3_1bpp", 0x41437F1B, false);
}

UTEST(pocket, hot_reload_without_a_reset)
{
    std::vector<uint8_t> first = read_rom("mode3_8bpp");
    ASSERT_TRUE(first.size() > 0);
    ASSERT_TRUE(boot_quiet(utest_result, first));
    run_and_check(utest_result, "mode3_8bpp", 0x29DB5FC9, false);

    std::vector<uint8_t> second = read_rom("mode3_1bpp");
    ASSERT_TRUE(second.size() > 0);
    /* The host asks to write the ROM slot before it writes the image, and
     * the running 6502 is in reset within a few clocks of the request,
     * before any of the new image lands. */
    ASSERT_TRUE((int)dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__resb);
    dut->dataslot_requestwrite_id = 0;
    dut->dataslot_requestwrite = 1;
    a_edge();
    a_edge();
    dut->dataslot_requestwrite = 0;
    for (int i = 0; i < 20; i++)
        a_edge();
    ASSERT_FALSE((int)dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__resb);
    host_load(second);

    ASSERT_TRUE(run_until_quiet(nullptr));
    run_and_check(utest_result, "mode3_1bpp", 0x41437F1B, false);
}

/* Whether the SDRAM holds the firmware's first bytes as the file has
 * them. They are the ROM block, which nothing stores to. */
static bool rom_holds(const std::vector<uint8_t> &fw, size_t n)
{
    auto &chip = dut->rootp->tb_pocket__DOT__chip__DOT__mem;
    for (size_t i = 0; i + 1 < n && i + 1 < fw.size(); i += 2)
        if (chip[(TB_STAGE_MEM_BASE + i) >> 1]
            != (uint16_t)(fw[i] | (fw[i + 1] << 8)))
            return false;
    return true;
}

/* The firmware's code is longer than this. */
#define FW_CODE 0x10000u

/* The firmware arrives as the host sends it, a data slot streamed over
 * the bridge into the SDRAM, and the soft CPU boots from there through
 * its cache. */
UTEST(pocket, the_firmware_boots_from_the_bridge)
{
    std::vector<uint8_t> fw = tb_stage_firmware(SW_BIN);
    ASSERT_GT(fw.size(), (size_t)FW_CODE);
    std::vector<uint8_t> rom = read_rom("mode3_1bpp");
    ASSERT_TRUE(rom.size() > 0);
    power_on(utest_result, false);
    ASSERT_FALSE(rom_holds(fw, FW_CODE));
    host_stream(fw, TB_STAGE_MEM_BASE);
    host_load(rom);
    dut->reset_n = 1;
    run_and_check(utest_result, "mode3_1bpp", 0x41437F1B);
    ASSERT_TRUE(rom_holds(fw, FW_CODE));
}

/* The host writes over the whole firmware while the machine runs. None
 * of it reaches the SDRAM, and the machine goes on to load another ROM,
 * fetching code its cache does not hold. */
UTEST(pocket, a_host_write_into_the_memory_is_dropped_while_running)
{
    std::vector<uint8_t> fw = tb_stage_firmware(SW_BIN);
    ASSERT_GT(fw.size(), (size_t)FW_CODE);
    std::vector<uint8_t> first = read_rom("mode3_8bpp");
    ASSERT_TRUE(first.size() > 0);
    ASSERT_TRUE(boot_quiet(utest_result, first));
    run_and_check(utest_result, "mode3_8bpp", 0x29DB5FC9, false);

    std::vector<uint8_t> junk(fw.size(), 0xFF);
    host_stream(junk, TB_STAGE_MEM_BASE);
    ASSERT_TRUE(rom_holds(fw, FW_CODE));

    std::vector<uint8_t> second = read_rom("mode3_1bpp");
    ASSERT_TRUE(second.size() > 0);
    host_load(second);
    ASSERT_TRUE(run_until_quiet(nullptr));
    run_and_check(utest_result, "mode3_1bpp", 0x41437F1B, false);
    ASSERT_TRUE(rom_holds(fw, FW_CODE));
}

/* A reset with the soft CPU's memory left unfit to run, both blocks,
 * then the firmware and a ROM loaded again over the bridge. The writes
 * land because the machine is in reset, and the soft CPU boots from them
 * with an empty cache and makes its RAM block again. */
UTEST(pocket, a_reset_and_a_reload_boot_clean)
{
    std::vector<uint8_t> fw = tb_stage_firmware(SW_BIN);
    ASSERT_GT(fw.size(), (size_t)FW_CODE);
    std::vector<uint8_t> first = read_rom("mode3_8bpp");
    ASSERT_TRUE(first.size() > 0);
    ASSERT_TRUE(boot_quiet(utest_result, first));
    run_and_check(utest_result, "mode3_8bpp", 0x29DB5FC9, false);

    dut->reset_n = 0;
    for (int i = 0; i < 40; i++)
        a_edge();
    ASSERT_EQ((int)dut->rootp->tb_pocket__DOT__core__DOT__run, 0);
    tb_stage_poke(dut->rootp->tb_pocket__DOT__chip__DOT__mem,
                  TB_STAGE_MEM_BASE,
                  std::vector<uint8_t>(TB_STAGE_MEM_SIZE, 0xFF));
    ASSERT_FALSE(rom_holds(fw, FW_CODE));

    host_stream(fw, TB_STAGE_MEM_BASE);
    std::vector<uint8_t> second = read_rom("mode3_1bpp");
    ASSERT_TRUE(second.size() > 0);
    host_load(second);
    dut->reset_n = 1;
    ASSERT_TRUE(run_until_quiet(nullptr));
    run_and_check(utest_result, "mode3_1bpp", 0x41437F1B, false);
    ASSERT_TRUE(rom_holds(fw, FW_CODE));
}

/* The image's register layout version, the word after its reset jump. */
#define FW_REG_VERSION 4u

static uint32_t fw_word(const std::vector<uint8_t> &fw, size_t at)
{
    return (uint32_t)fw[at] | ((uint32_t)fw[at + 1] << 8)
           | ((uint32_t)fw[at + 2] << 16) | ((uint32_t)fw[at + 3] << 24);
}

/* A firmware built for another bitstream stops before it uses a register.
 * The 6502 stays in reset, nothing is said on either console, and the
 * screen stays black. A firmware that runs on logs within its first
 * frames. */
UTEST(pocket, a_firmware_of_another_register_layout_stops)
{
    std::vector<uint8_t> fw = tb_stage_firmware(SW_BIN);
    ASSERT_GT(fw.size(), (size_t)FW_CODE);
    ASSERT_EQ((uint32_t)POCKET_REG_VERSION, fw_word(fw, FW_REG_VERSION));
    fw[FW_REG_VERSION] ^= 1;
    std::vector<uint8_t> rom = read_rom("mode3_1bpp");
    ASSERT_TRUE(rom.size() > 0);
    power_on(utest_result, false);
    tb_stage_poke(dut->rootp->tb_pocket__DOT__chip__DOT__mem,
                  TB_STAGE_MEM_BASE, fw);
    host_load(rom);
    dut->reset_n = 1;

    bool said = false, ran = false, lit = false;
    int frames = 0;
    bool was_vs = false;
    while (frames < 4)
    {
        long sys_before = tb.sys;
        tick();
        if (tb.sys == sys_before)
            continue;
        said |= dut->tb_pocket_tx_valid || dut->tb_pocket_rv_tx_valid;
        ran |= (bool)dut->rootp->tb_pocket__DOT__core__DOT__machine__DOT__resb;
        lit |= dut->tb_pocket_de && !dut->tb_pocket_skip && dut->tb_pocket_rgb;
        if (dut->tb_pocket_vs && !was_vs)
            frames++;
        was_vs = dut->tb_pocket_vs;
    }
    ASSERT_FALSE(said);
    ASSERT_FALSE(ran);
    ASSERT_FALSE(lit);
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

static std::vector<uint8_t> gamepad_mapper(uint16_t at)
{
    tb_asm p;
    /* For xreg device 0, channel 0, index 2 the word is the XRAM
     * address of the gamepad block, which holds four 10-byte records. */
    p.push(0);
    p.push(0);
    p.push(2);
    p.pushw(at);
    p.call(0x01);
    uint16_t here = p.here();
    p.jmp(here);

    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, TB_ORG, p.b.data(), p.b.size());
    const uint8_t reset[] = {(uint8_t)TB_ORG, (uint8_t)(TB_ORG >> 8)};
    tb_rom_record(rom, 0xFFFC, reset, sizeof reset);
    return rom;
}

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
    /* Bits 31:28 of a controller's key word are its type, and apf.c
     * mounts type 2 as a gamepad. */
    static const uint32_t TYPE_PAD = 2u << 28;

    power_on(utest_result);
    host_load(gamepad_mapper(AT));
    dut->reset_n = 1;
    run_frames(10);

    ASSERT_EQ((uint32_t)xram_at(AT), 0u);

    /* Key bit 1 is down on the d-pad and key bit 4 is A. */
    dut->cont1_key = TYPE_PAD | (1u << 1) | (1u << 4);
    run_frames(10);
    ASSERT_EQ((uint32_t)(xram_at(AT) & 0x80), 0x80u);   /* connected */
    ASSERT_EQ((uint32_t)(xram_at(AT) & 0x0F), 0x02u);   /* dpad down */
    ASSERT_EQ((uint32_t)(xram_at(AT + 2) & 0x01), 0x01u); /* button0: A */

    dut->cont1_key = TYPE_PAD | (1u << 4);
    run_frames(10);
    ASSERT_EQ((uint32_t)(xram_at(AT) & 0x0F), 0x00u);
    ASSERT_EQ((uint32_t)(xram_at(AT + 2) & 0x01), 0x01u);

    ASSERT_EQ((uint32_t)xram_at(AT + 10), 0u);

    dut->cont1_key = 0;
    run_frames(20);
    for (int i = 0; i < 10; i++)
        ASSERT_EQ((uint32_t)xram_at(AT + i), 0u);
}
