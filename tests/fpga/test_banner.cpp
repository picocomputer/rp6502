/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The M3 frame test: one .rp6502 banner, two machines, pixel-exact. The
 * ROM clears the screen and hides the cursor so both sides settle to the
 * same picture regardless of their histories, then prints plain text,
 * colors, reverse, and underline. The RTL frame is captured off the
 * composed pixel stream and compared word for word against the oracle's
 * framebuffer through the same RGB555-to-RGBA8 conversion.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_quiet.h"
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

static void rom_record(std::vector<uint8_t> &rom, uint16_t addr,
                       const uint8_t *data, size_t len)
{
    char line[64];
    snprintf(line, sizeof(line), "$%04X $%zX $%08X\n",
             addr, len, crc32_buf(data, len));
    rom.insert(rom.end(), line, line + strlen(line));
    rom.insert(rom.end(), data, data + len);
}

/* rgb555_to_rgba8, byte for byte the emulator's (emu/sys/vga.c). */
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

/* The banner: reset SGR, clear, home, hide the cursor, then exercise
 * text, color, reverse, and underline. Printed under the $FFE0 ready
 * bit like every wire byte. */
static const char banner[] =
    "\33[0m\33[2J\33[H\33[?25l"
    "RP6502 openFPGA\r\n"
    "\33[31mred\33[32m green \33[1;34mbold blue\33[0m\r\n"
    "\33[7mreverse\33[0m \33[4munderline\33[0m plain\r\n";

UTEST(banner, frame_matches_oracle)
{
    /* prog: ldx #0; loop: lda msg,x; beq done; wait: bit $FFE0; bpl wait;
     * sta $FFE1; inx; bne loop; nop; done: stp; msg follows. */
    std::vector<uint8_t> prog = {
        0xA2, 0x00,
        0xBD, 0x14, 0x03,
        0xF0, 0x0C,
        0x2C, 0xE0, 0xFF,
        0x10, 0xFB,
        0x8D, 0xE1, 0xFF,
        0xE8,
        0xD0, 0xF0,
        0xEA,
        0xDB,
    };
    prog.insert(prog.end(), banner, banner + sizeof(banner));
    static const uint8_t vectors[] = {0x00, 0x03};

    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, 0x0300, prog.data(), prog.size());
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    /* The oracle runs it first and settles. */
    const char *path = TEST_SCRATCH "/test_banner.rp6502";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);
    oracle_init();
    ASSERT_TRUE(oracle_restart(path));
    oracle_run_frames(30);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    ASSERT_EQ(ow, 640);
    ASSERT_EQ(oh, 480);
    const uint32_t *ofb = oracle_framebuffer();

    /* The RTL machine boots the same image and quiet-exits; the scanout
     * keeps painting the latched frame. */
    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
    }
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    ASSERT_TRUE(tb_quiet(dut, [&] {
        uint32_t a = dut->rp6502_stage_addr;
        dut->stage_rdata = a < rom.size() ? rom[a] : 0;
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
    }));

    /* Capture two whole frames off the pixel stream and demand they are
     * identical (settled) before comparing against the oracle. */
    static uint32_t fb[2][640 * 480];
    for (int frame = 0; frame < 2; frame++)
    {
        /* Align to a frame boundary. */
        while (dut->rp6502_scanline != 524)
        {
            dut->clk_sys = 1;
            dut->eval();
            dut->clk_sys = 0;
            dut->eval();
        }
        while (dut->rp6502_scanline != 0)
        {
            dut->clk_sys = 1;
            dut->eval();
            dut->clk_sys = 0;
            dut->eval();
        }
        size_t at = 0;
        while (at < 640 * 480)
        {
            dut->clk_sys = 1;
            dut->eval();
            dut->clk_sys = 0;
            dut->eval();
            if (dut->rp6502_vid_de)
                fb[frame][at++] = rgba8(dut->rp6502_vid_pixel);
        }
    }
    ASSERT_EQ(memcmp(fb[0], fb[1], sizeof(fb[0])), 0);

    int diffs = 0;
    for (size_t p = 0; p < 640 * 480; p++)
        if (fb[0][p] != ofb[p])
        {
            if (getenv("BANNER_DEBUG") && diffs < 16)
                fprintf(stderr, "diff at x=%zu y=%zu rtl=%08X oracle=%08X\n",
                        p % 640, p / 640, fb[0][p], ofb[p]);
            diffs++;
        }
    ASSERT_EQ(diffs, 0);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
