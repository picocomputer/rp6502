/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Mode 3 bitmaps against the oracle, one canvas geometry and depth at a
 * time: the bitmap, palette, and config ride in as XRAM records, the 6502
 * programs the canvas and the mode through op 0x01, and both machines
 * settle. The oracle renders canvas-native; the RTL does the doubling and
 * letterboxing itself, so the comparison maps rtl(x, y) onto
 * oracle(x >> xs, (y - yo) >> ys) inside the picture and demands opaque
 * black outside it.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

static bool load_firmware(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    auto &tcm = dut->rootp->rp6502__DOT__rv__DOT__tcm;
    for (size_t i = 0; i < 32768; i++)
        tcm[i] = 0;
    uint8_t buf[4];
    size_t word = 0, n;
    while ((n = fread(buf, 1, 4, f)) > 0 && word < 32768)
    {
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++)
            v |= (uint32_t)buf[i] << (8 * i);
        tcm[word++] = v;
    }
    fclose(f);
    return true;
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void rom_record(std::vector<uint8_t> &rom, uint32_t addr,
                       const uint8_t *data, size_t len)
{
    char line[64];
    snprintf(line, sizeof(line), "$%05X $%zX $%08X\n",
             addr, len, crc32_buf(data, len));
    rom.insert(rom.end(), line, line + strlen(line));
    rom.insert(rom.end(), data, data + len);
}

static uint32_t rgba8(uint16_t px)
{
    uint32_t r5 = px & 0x1F;
    uint32_t g5 = (px >> 6) & 0x1F;
    uint32_t b5 = (px >> 11) & 0x1F;
    uint32_t r = (r5 << 3) | (r5 >> 2);
    uint32_t g = (g5 << 3) | (g5 >> 2);
    uint32_t b = (b5 << 3) | (b5 >> 2);
    return r | (g << 8) | (b << 16) | 0xFF000000u;
}

static void clock_cycle()
{
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_sys = 0;
    dut->eval();
}

static void capture_frame(uint32_t *fb)
{
    while (dut->rp6502_scanline != 524)
        clock_cycle();
    while (dut->rp6502_scanline != 0)
        clock_cycle();
    size_t at = 0;
    while (at < 640 * 480)
    {
        clock_cycle();
        if (dut->rp6502_vid_de)
            fb[at++] = rgba8(dut->rp6502_vid_pixel);
    }
}

/* The 6502: canvas, then the mode 3 program, then stop. */
static std::vector<uint8_t> prog_mode3(uint8_t canvas, uint16_t attr,
                                       uint16_t config_ptr)
{
    std::vector<uint8_t> p;
    auto lda = [&](uint8_t v) { p.insert(p.end(), {0xA9, v}); };
    auto sta = [&](uint16_t a) {
        p.insert(p.end(), {0x8D, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto push = [&](uint8_t v) { lda(v); sta(0xFFEC); };
    auto pushw = [&](uint16_t w) { push((uint8_t)(w >> 8)); push((uint8_t)w); };
    auto op1 = [&]() {
        lda(0x01);
        sta(0xFFEF);
        p.insert(p.end(), {0x20, 0xF1, 0xFF});
    };
    push(1); push(0); push(0); pushw(canvas);
    op1();
    push(1); push(0); push(1);
    pushw(3); pushw(attr); pushw(config_ptr);
    pushw(0); pushw(0); pushw(0); /* plane 0, whole canvas */
    op1();
    p.push_back(0xDB);
    return p;
}

/* One case end to end: build, run both machines, settle, compare. */
static void run_case(int *utest_result,
                     const char *name, uint8_t canvas,
                     uint16_t attr, int bpp,
                     int16_t bm_w, int16_t bm_h,
                     int16_t x_pos, int16_t y_pos, bool xram_palette)
{
    const uint16_t config_ptr = 0x0100;
    const uint16_t palette_ptr = xram_palette ? 0x0200 : 0xFFFF;
    const uint16_t data_ptr = 0x0800;

    uint8_t cfg[14];
    cfg[0] = 0; /* x_wrap */
    cfg[1] = 0; /* y_wrap */
    cfg[2] = (uint8_t)x_pos;
    cfg[3] = (uint8_t)(x_pos >> 8);
    cfg[4] = (uint8_t)y_pos;
    cfg[5] = (uint8_t)(y_pos >> 8);
    cfg[6] = (uint8_t)bm_w;
    cfg[7] = (uint8_t)(bm_w >> 8);
    cfg[8] = (uint8_t)bm_h;
    cfg[9] = (uint8_t)(bm_h >> 8);
    cfg[10] = (uint8_t)data_ptr;
    cfg[11] = (uint8_t)(data_ptr >> 8);
    cfg[12] = (uint8_t)palette_ptr;
    cfg[13] = (uint8_t)(palette_ptr >> 8);

    size_t bm_bytes = ((size_t)bm_w * bpp + 7) / 8 * bm_h;
    std::vector<uint8_t> bitmap(bm_bytes);
    for (size_t i = 0; i < bm_bytes; i++)
        bitmap[i] = (uint8_t)(i * 13 + 7);

    std::vector<uint8_t> pal;
    if (xram_palette)
        for (int i = 0; i < (1 << bpp); i++)
        {
            uint16_t c = (uint16_t)(0x0020 | (i * 2657));
            pal.push_back((uint8_t)c);
            pal.push_back((uint8_t)(c >> 8));
        }

    std::vector<uint8_t> prog = prog_mode3(canvas, attr, config_ptr);
    static const uint8_t vectors[] = {0x00, 0x03};
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, 0x0300, prog.data(), prog.size());
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    rom_record(rom, 0x10000u + config_ptr, cfg, sizeof(cfg));
    if (xram_palette)
        rom_record(rom, 0x10000u + palette_ptr, pal.data(), pal.size());
    rom_record(rom, 0x10000u + data_ptr, bitmap.data(), bitmap.size());

    char path[64];
    snprintf(path, sizeof(path), "test_mode3_%s.rp6502", name);
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);
    ASSERT_TRUE(oracle_restart(path));
    oracle_run_frames(30);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    const uint32_t *ofb = oracle_framebuffer();

    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    bool stopped = false;
    for (int i = 0; i < 8000000; i++)
    {
        uint32_t a = dut->rp6502_stage_addr;
        dut->stage_rdata = a < rom.size() ? rom[a] : 0;
        clock_cycle();
        stopped = dut->rootp->rp6502__DOT__cpu__DOT__stop_flag != 0;
        if (dut->rp6502_rv_halted && stopped)
            break;
    }
    ASSERT_TRUE(dut->rp6502_rv_halted);
    ASSERT_TRUE(stopped);

    if (getenv("MODE3_PROBE"))
    {
        fprintf(stderr, "canvas=%d prog0=%08X prog0c=%08X\n",
                (int)dut->rootp->rp6502__DOT__vid_prog__DOT__canvas_shadow,
                dut->rootp->rp6502__DOT__vid_prog__DOT__prog[0],
                dut->rootp->rp6502__DOT__vid_prog__DOT__prog[1]);
        auto *r = dut->rootp;
        for (int i = 0; i < 900000; i++)
        {
            clock_cycle();
            if ((r->rp6502__DOT__vid_h == 400 || r->rp6502__DOT__vid_h == 700) && r->rp6502__DOT__vid_v >= (uint32_t)atoi(getenv("MODE3_PROBE")) && r->rp6502__DOT__vid_v <= (uint32_t)atoi(getenv("MODE3_PROBE")) + 6)
                fprintf(stderr, "v=%d h=%d st=%d attr=%04X px=%d f0=%d f1=%d\n",
                        (int)r->rp6502__DOT__vid_v,
                        (int)r->rp6502__DOT__vid_h,
                        (int)r->rp6502__DOT__gen_mode__BRA__0__KET____DOT__vid_mode__DOT__state,
                        r->rp6502__DOT__gen_mode__BRA__0__KET____DOT__vid_mode__DOT__attr,
                        (int)r->rp6502__DOT__gen_mode__BRA__0__KET____DOT__vid_mode__DOT__px,
                        (int)r->rp6502__DOT__gen_mode__BRA__0__KET____DOT__vid_mode__DOT__filled_q[0],
                        (int)r->rp6502__DOT__gen_mode__BRA__0__KET____DOT__vid_mode__DOT__filled_q[1]);
        }
    }

    static uint32_t fb[2][640 * 480];
    capture_frame(fb[0]);
    capture_frame(fb[1]);
    ASSERT_EQ(memcmp(fb[0], fb[1], sizeof(fb[0])), 0);

    int xs = ow == 320 ? 1 : 0;
    int ys = oh == 480 ? 0 : 1;
    int yo = (oh == 180 || oh == 360) ? 60 : 0;
    if (oh == 360)
        ys = 1;
    int diffs = 0;
    for (int y = 0; y < 480; y++)
        for (int x = 0; x < 640; x++)
        {
            uint32_t want;
            int cy = (y - yo) >> ys;
            if (y < yo || cy >= oh)
                want = 0xFF000000u;
            else
                want = ofb[(size_t)cy * ow + (x >> xs)];
            if (fb[0][(size_t)y * 640 + x] != want)
            {
                if (getenv("MODE3_DEBUG") && diffs < 20)
                    fprintf(stderr, "%s diff x=%d y=%d rtl=%08X want=%08X\n",
                            name, x, y, fb[0][(size_t)y * 640 + x], want);
                diffs++;
            }
        }
    ASSERT_EQ(diffs, 0);
}

UTEST(mode3, bpp8_xram_palette_640x480)
{
    run_case(utest_result, "8bpp", 3, 3, 8, 64, 64, 10, 20, true);
}

UTEST(mode3, bpp1_builtin_320x240)
{
    run_case(utest_result, "1bpp", 1, 0, 1, 64, 48, 5, 7, false);
}

UTEST(mode3, bpp4_reversed_320x180)
{
    run_case(utest_result, "4bppr", 2, 10, 4, 40, 30, 0, 0, false);
}

UTEST(mode3, bpp16_640x360)
{
    run_case(utest_result, "16bpp", 4, 4, 16, 32, 16, 100, 50, false);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    oracle_init();
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
