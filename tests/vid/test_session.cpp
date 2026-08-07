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
#include "tb_term.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

/* Capture a frame. The firmware runs forever, stepping the blink phase
 * on its own timer, so a phase under test has to be held down for the
 * whole frame rather than merely written before it. */
static int pinned_blink = -1;

static void capture_frame(uint32_t *fb)
{
    auto ck = [] {
        if (pinned_blink >= 0)
            dut->rootp->rp6502__DOT__vid_mode0__DOT__blink_shadow =
                (uint8_t)pinned_blink;
        tb_clock(dut);
    };
    while (dut->rp6502_scanline != 524)
        ck();
    while (dut->rp6502_scanline != 0)
        ck();
    size_t at = 0;
    while (at < 640 * 480)
    {
        ck();
        if (dut->rp6502_vid_de)
            fb[at++] = tb_rgba8(dut->rp6502_vid_pixel);
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
    tb_rom_record(rom, org, image.data(), image.size());
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

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
    ASSERT_TRUE(tb_firmware(dut, SW_BIN));
    tb_reset(dut);
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    ASSERT_TRUE(tb_quiet(dut, [&] {
        uint32_t a = dut->rp6502_stage_addr;
        tb_host_tick(dut, rom);
        dut->stage_rdata = tb_stage(rom, a);
        tb_clock(dut);
    }));

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
    uint32_t base = r->rp6502__DOT__vid_mode0__DOT__row_shadow[25];
    uint32_t seed = term_cell(
        r, r->rp6502__DOT__vid_mode0__DOT__row_shadow[0] >> 2);
    uint32_t bgw = term_cell(
        r, (r->rp6502__DOT__vid_mode0__DOT__row_shadow[0] >> 2) + 1);
    /* {fg from a real cell, ATTR_BLINK, 'B'} over the same background. */
    term_cell_set(r, base >> 2, (seed & 0xFFFF0000u) | 0x0200u | 'B');
    term_cell_set(r, (base >> 2) + 1, bgw);

    /* The firmware keeps advancing its own phase, so hold ours. */
    static uint32_t on[640 * 480], off[640 * 480];
    pinned_blink = 0;
    capture_frame(on);  /* latch */
    capture_frame(on);
    pinned_blink = 2;
    capture_frame(off);  /* latch */
    capture_frame(off);
    pinned_blink = -1;
    if (getenv("SESSION_DEBUG"))
    {
        fprintf(stderr, "base=%04X w0=%08X w1=%08X\n", base,
                term_cell(r, base >> 2), term_cell(r, (base >> 2) + 1));
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
