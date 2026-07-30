/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The glyphs, end to end: the asset beside the core reaches the store
 * through the firmware's own copy, and a program that asks for a code
 * page gets one. The oracle builds the same tables at runtime with
 * font.c, so the store is compared against those rather than against the
 * generator that produced the asset — a check the two sides cannot both
 * be wrong about.
 *
 * The store is only reachable by peeking the fabric because nothing
 * reads it back; the terminal renders from it and that is all. So the
 * faces the terminal never shows — italic, the DEC graphics — are
 * checked here and nowhere else.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_quiet.h"
#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

extern "C" {
#include "vga/term/font.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;
/* Half clk_sys, rising with it: the PLL's shape, not a divider's. */
static bool rv_phase;

static void clock_cycle()
{
    rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
    dut->eval();
    dut->clk_rv = 0;
    dut->clk_sys = 0;
    dut->eval();
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

/* A program that asks for one code page and stops: the two API
 * registers, the operation, the trampoline. */
static std::vector<uint8_t> rom_code_page(uint16_t cp)
{
    const uint8_t prog[] = {
        0xA9, (uint8_t)(cp & 0xFF),  /* lda #cp_lo    */
        0x8D, 0xF4, 0xFF,            /* sta $FFF4     ; API_A  */
        0xA9, (uint8_t)(cp >> 8),    /* lda #cp_hi    */
        0x8D, 0xF6, 0xFF,            /* sta $FFF6     ; API_X  */
        0xA9, 0x03,                  /* lda #3        */
        0x8D, 0xEF, 0xFF,            /* sta $FFEF     ; API_OP */
        0x20, 0xF1, 0xFF,            /* jsr $FFF1     */
        0xDB,                        /* stp           */
    };
    const uint8_t vectors[] = {0x00, 0x03};
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, 0x0300, prog, sizeof(prog));
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return rom;
}

/* The store answers a word at a time; the faces are word arrays in the
 * fabric, so a byte is a lane of one. */
template <typename Face>
static uint8_t face_byte(Face &face, size_t at)
{
    return (uint8_t)(face[at / 4] >> (8 * (at % 4)));
}

static bool boot(const std::vector<uint8_t> &rom)
{
    auto *r = dut->rootp;
    if (!tb_load_tcm(r->rp6502__DOT__rv__DOT__tcm0,
                     r->rp6502__DOT__rv__DOT__tcm1,
                     r->rp6502__DOT__rv__DOT__tcm2,
                     r->rp6502__DOT__rv__DOT__tcm3, SW_BIN))
        return false;
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
        clock_cycle();
    dut->rst_n = 1;
    r->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();
    return tb_quiet(dut, [&] {
        dut->stage_rdata = tb_stage(rom, dut->rp6502_stage_addr);
        clock_cycle();
    });
}

UTEST(fontstore, boot_image_matches_font_init)
{
    ASSERT_TRUE(boot(rom_code_page(437)));
    font_init();
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f8, i), font8[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__ital, i),
                  italic16[i]);
    for (size_t i = 0; i < 512; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__dec, i),
                  font_dec_16[i]);
}

/* Every page font.c accepts, against the tables font.c itself builds for
 * it. The low halves must survive the change untouched — a page rewrites
 * the high half alone, and writing the whole face would be the easy way
 * to pass this while doing four times the work. */
static void one_page(int *utest_result, uint16_t cp)
{
    ASSERT_TRUE(boot(rom_code_page(cp)));
    font_init();
    font_set_code_page(cp);
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f8, i), font8[i]);
}

UTEST(fontstore, code_page_720) { one_page(utest_result, 720); }
UTEST(fontstore, code_page_775) { one_page(utest_result, 775); }
UTEST(fontstore, code_page_866) { one_page(utest_result, 866); }
UTEST(fontstore, code_page_869) { one_page(utest_result, 869); }

/* A page nobody carries blanks the high half rather than keeping the
 * one before it, which is font_set_code_page's rule. */
UTEST(fontstore, unknown_code_page_blanks_the_high_half)
{
    ASSERT_TRUE(boot(rom_code_page(999)));
    auto *r = dut->rootp;
    for (int row = 0; row < 16; row++)
        for (int code = 128; code < 256; code++)
            ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f16,
                                (size_t)row * 256 + code), 0);
    for (int row = 0; row < 8; row++)
        for (int code = 128; code < 256; code++)
            ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f8,
                                (size_t)row * 256 + code), 0);
    /* and the ASCII half is still there */
    font_init();
    for (int row = 0; row < 16; row++)
        for (int code = 0; code < 128; code++)
            ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f16,
                                (size_t)row * 256 + code),
                      font16[row * 256 + code]);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    oracle_init();
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
