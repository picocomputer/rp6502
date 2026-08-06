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
#include "tb_host.h"
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

static std::vector<uint8_t> rom_image(const std::vector<uint8_t> &prog)
{
    const uint8_t vectors[] = {0x00, 0x03};
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, 0x0300, prog.data(), prog.size());
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return rom;
}

/* A program that asks for one code page and stops: the two API
 * registers, the operation, the trampoline, and the answer to the
 * console so the bench reads the page the program was given. */
static std::vector<uint8_t> rom_code_page(uint16_t cp)
{
    const std::vector<uint8_t> prog = {
        0xA9, (uint8_t)(cp & 0xFF),  /* lda #cp_lo    */
        0x8D, 0xF4, 0xFF,            /* sta $FFF4     ; API_A  */
        0xA9, (uint8_t)(cp >> 8),    /* lda #cp_hi    */
        0x8D, 0xF6, 0xFF,            /* sta $FFF6     ; API_X  */
        0xA9, 0x03,                  /* lda #3        */
        0x8D, 0xEF, 0xFF,            /* sta $FFEF     ; API_OP */
        0x20, 0xF1, 0xFF,            /* jsr $FFF1     */
        0x8D, 0xE1, 0xFF,            /* sta $FFE1     ; page lo */
        0x8E, 0xE1, 0xFF,            /* stx $FFE1     ; page hi */
        0xDB,                        /* stp           */
    };
    return rom_image(prog);
}

/* The path the libraries actually take: code_page() is ria_attr_set
 * followed by ria_attr_get. The value is a 32-bit xstack push, most
 * significant byte first so the least significant is the one on top,
 * which is what api_pop_uint32_end reads. Every answer goes to the
 * console, A then X, so the bench reads exactly what the program read. */
static std::vector<uint8_t> rom_attr_code_page(const std::vector<uint16_t> &sets)
{
    std::vector<uint8_t> p;
    auto lda = [&](uint8_t v) { p.insert(p.end(), {0xA9, v}); };
    auto sta = [&](uint16_t a) {
        p.insert(p.end(), {0x8D, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto push = [&](uint8_t v) { lda(v); sta(0xFFEC); };
    auto call = [&](uint8_t op) {
        lda(0x02); /* RIA_ATTR_CODE_PAGE */
        sta(0xFFF4);
        lda(op);
        sta(0xFFEF);
        p.insert(p.end(), {0x20, 0xF1, 0xFF}); /* jsr $FFF1 */
        sta(0xFFE1);
        p.insert(p.end(), {0x8E, 0xE1, 0xFF});
    };
    for (uint16_t cp : sets)
    {
        push(0);
        push(0);
        push((uint8_t)(cp >> 8));
        push((uint8_t)cp);
        call(0x0B);
    }
    call(0x0A);
    p.push_back(0xDB); /* stp */
    return rom_image(p);
}

/* The store answers a word at a time; the faces are word arrays in the
 * fabric, so a byte is a lane of one. */
template <typename Face>
static uint8_t face_byte(Face &face, size_t at)
{
    return (uint8_t)(face[at / 4] >> (8 * (at % 4)));
}

/* The console is the 6502's own TX register; the firmware's printf goes
 * out a separate one, so what lands in `out` is the program's alone. */
static bool boot(const std::vector<uint8_t> &rom, std::string *out = nullptr)
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
        tb_host_tick(dut, rom);
        dut->stage_rdata = tb_stage(rom, dut->rp6502_stage_addr);
        clock_cycle();
        if (out && dut->rp6502_tx_valid)
            out->push_back((char)dut->rp6502_tx_data);
    });
}

/* The page a program was handed back, as its two console bytes. */
static uint16_t reported(const std::string &out, size_t at)
{
    return (uint16_t)((uint8_t)out[at] | ((uint8_t)out[at + 1] << 8));
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
    std::string out;
    ASSERT_TRUE(boot(rom_code_page(cp), &out));
    ASSERT_EQ(out.size(), (size_t)2);
    ASSERT_EQ(reported(out, 0), cp);
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

/* A page nobody carries changes nothing. This blanked the high half
 * until the API learned to filter: font_set_code_page still blanks for a
 * page with no glyphs — the store's rule, and the VGA chip's — but that
 * is the platform's business now and not something a program can ask
 * for. The RIA refuses the same request through f_setcp, whose valid
 * list is these same seventeen pages. */
UTEST(fontstore, unknown_code_page_keeps_the_one_in_force)
{
    std::string out;
    ASSERT_TRUE(boot(rom_code_page(999), &out));
    ASSERT_EQ(out.size(), (size_t)2);
    ASSERT_EQ(reported(out, 0), (uint16_t)437);
    font_init();
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f8, i), font8[i]);
}

/* The bug this all came from: a program that asks the attribute API what
 * page it is on was told -1, and skipped its CP437 path on a machine
 * that was already on 437. */
UTEST(fontstore, attribute_get_reports_the_boot_default)
{
    std::string out;
    ASSERT_TRUE(boot(rom_attr_code_page({}), &out));
    ASSERT_EQ(out.size(), (size_t)2);
    ASSERT_EQ(reported(out, 0), (uint16_t)437);
}

/* One boot, four claims: the attribute set moves the glyphs, a page the
 * asset does not carry is a no-op rather than a refusal, the set still
 * answers success, and the get afterwards names the page actually in
 * force. The store must hold 850's — not blanks, which is what the
 * second set used to leave, and not 437's, which is what a revert
 * would. */
UTEST(fontstore, attribute_set_takes_a_page_and_ignores_the_rest)
{
    std::string out;
    ASSERT_TRUE(boot(rom_attr_code_page({850, 1252}), &out));
    ASSERT_EQ(out.size(), (size_t)6);
    ASSERT_EQ(reported(out, 0), (uint16_t)0); /* both sets succeeded */
    ASSERT_EQ(reported(out, 2), (uint16_t)0);
    ASSERT_EQ(reported(out, 4), (uint16_t)850);
    font_init();
    font_set_code_page(850);
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->rp6502__DOT__vid_font__DOT__f8, i), font8[i]);
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
