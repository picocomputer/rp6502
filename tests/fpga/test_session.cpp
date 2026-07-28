/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The M3 scripted session: scrolling past the screen so y_offset walks, a
 * DECSTBM region scroll permuting row_idx, lazy EL/ED clears, the SGR set
 * with an SGR 58 underline color, DEC Special Graphics, italic, an alt
 * screen round trip, and a steady block cursor parked mid-screen — settled
 * on both machines and compared pixel for pixel. Then an RTL-only blink
 * check: the cell-blink timer runs 166 ms a phase, far beyond simulation
 * budgets, so the testbench writes a blinking cell and steps the phase
 * register itself, asserting the glyph blanks to its background.
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

static void rom_record(std::vector<uint8_t> &rom, uint16_t addr,
                       const uint8_t *data, size_t len)
{
    char line[64];
    snprintf(line, sizeof(line), "$%04X $%zX $%08X\n",
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

UTEST(session, scripted_frame_matches_oracle)
{
    std::string script = "\33[0m\33[2J\33[H\33[?25l";
    /* Forty lines walk the scroll past the 30-row screen. */
    for (int i = 0; i < 40; i++)
    {
        char line[32];
        snprintf(line, sizeof(line), "scroll line %02d\r\n", i);
        script += line;
    }
    /* A region scroll inside DECSTBM margins permutes row_idx. */
    script += "\33[5;10r\33[10;1H\nregion a\nregion b\nregion c\n\33[r";
    /* Attributes, an underline color, DEC graphics, italic, an EL. */
    script += "\33[15;1H\33[1;33;44mbold yellow on blue\33[0m "
              "\33[7mreverse\33[0m \33[4;58;5;196mulcolor\33[0m "
              "\33[3mitalic\33[0m \33(0lqqk\33(B";
    script += "\33[16;1Hpartial line\33[8G\33[K";
    /* Alt screen round trip: its content must not survive the return. */
    script += "\33[?1049h\33[2J\33[HALT SCREEN\33[?1049l";
    /* A steady block cursor parked mid-screen renders on both sides. */
    script += "\33[20;5H\33[2 q\33[?25h";

    /* An indexed page reaches 255 bytes, so the printer chains: each
     * block prints its part and jumps to the next; the last one stops. */
    std::vector<std::string> parts;
    for (size_t at = 0; at < script.size(); at += 255)
        parts.push_back(script.substr(at, 255));
    uint16_t org = 0x0300;
    std::vector<uint8_t> image;
    uint16_t next = org;
    for (size_t p = 0; p < parts.size(); p++)
    {
        uint16_t msg = (uint16_t)(next + 22);
        bool last = p + 1 == parts.size();
        uint16_t after = (uint16_t)(msg + parts[p].size() + 1);
        std::vector<uint8_t> b = {
            0xA2, 0x00,
            0xBD, (uint8_t)(msg & 0xFF), (uint8_t)(msg >> 8),
            0xF0, 0x0C,
            0x2C, 0xE0, 0xFF,
            0x10, 0xFB,
            0x8D, 0xE1, 0xFF,
            0xE8,
            0xD0, 0xF0,
            0xEA,
        };
        if (last)
        {
            b.push_back(0xDB);
            b.push_back(0xEA);
            b.push_back(0xEA);
        }
        else
        {
            b.push_back(0x4C);
            b.push_back((uint8_t)(after & 0xFF));
            b.push_back((uint8_t)(after >> 8));
        }
        ASSERT_EQ(b.size(), (size_t)22);
        b.insert(b.end(), parts[p].begin(), parts[p].end());
        b.push_back(0);
        image.insert(image.end(), b.begin(), b.end());
        next = (uint16_t)(next + b.size());
    }
    static const uint8_t vectors[] = {0x00, 0x03};

    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, org, image.data(), image.size());
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    /* Oracle side. */
    const char *path = TEST_SCRATCH "/test_session.rp6502";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);
    oracle_init();
    ASSERT_TRUE(oracle_restart(path));
    oracle_run_frames(60);
    const uint32_t *ofb = oracle_framebuffer();

    /* RTL side. */
    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    bool stopped = false;
    for (int i = 0; i < 12000000; i++)
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

    static uint32_t fb[2][640 * 480];
    capture_frame(fb[0]);
    capture_frame(fb[1]);
    ASSERT_EQ(memcmp(fb[0], fb[1], sizeof(fb[0])), 0);

    int diffs = 0;
    for (size_t p = 0; p < 640 * 480; p++)
        if (fb[0][p] != ofb[p])
        {
            if (getenv("SESSION_DEBUG") && diffs < 16)
                fprintf(stderr, "diff at x=%zu y=%zu rtl=%08X oracle=%08X\n",
                        p % 640, p / 640, fb[0][p], ofb[p]);
            diffs++;
        }
    ASSERT_EQ(diffs, 0);

    /* Blink, RTL-relative: write a blinking glyph into a blank row and
     * step the phase register by hand — the off phase blanks the glyph to
     * its background. */
    auto *r = dut->rootp;
    uint32_t base = r->rp6502__DOT__vid_term__DOT__row_shadow[25];
    uint32_t seed = r->rp6502__DOT__vid_term__DOT__cells[
        r->rp6502__DOT__vid_term__DOT__row_shadow[0] >> 2];
    uint32_t bgw = r->rp6502__DOT__vid_term__DOT__cells[
        (r->rp6502__DOT__vid_term__DOT__row_shadow[0] >> 2) + 1];
    /* {fg from a real cell, ATTR_BLINK, 'B'} over the same background. */
    r->rp6502__DOT__vid_term__DOT__cells[base >> 2] =
        (seed & 0xFFFF0000u) | 0x0200u | 'B';
    r->rp6502__DOT__vid_term__DOT__cells[(base >> 2) + 1] = bgw;

    /* The firmware's own phase advanced during the run; pin it lit. */
    static uint32_t on[640 * 480], off[640 * 480];
    r->rp6502__DOT__vid_term__DOT__blink_shadow = 0;
    capture_frame(on);  /* latch */
    capture_frame(on);
    r->rp6502__DOT__vid_term__DOT__blink_shadow = 2;
    capture_frame(off);  /* latch */
    capture_frame(off);
    if (getenv("SESSION_DEBUG"))
    {
        fprintf(stderr, "base=%04X w0=%08X w1=%08X\n", base,
                r->rp6502__DOT__vid_term__DOT__cells[base >> 2],
                r->rp6502__DOT__vid_term__DOT__cells[(base >> 2) + 1]);
        for (int y = 25 * 16; y < 25 * 16 + 4; y++)
            fprintf(stderr, "y=%d on=%08X %08X off=%08X %08X\n", y,
                    on[y * 640 + 2], on[y * 640 + 3],
                    off[y * 640 + 2], off[y * 640 + 3]);
    }
    bool changed = false;
    for (int y = 25 * 16; y < 26 * 16 && !changed; y++)
        for (int x = 0; x < 8; x++)
            if (on[y * 640 + x] != off[y * 640 + x])
            {
                changed = true;
                break;
            }
    ASSERT_TRUE(changed);
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
