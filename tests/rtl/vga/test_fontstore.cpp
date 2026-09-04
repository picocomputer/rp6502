/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The glyphs, end to end: the asset beside the core reaches the store
 * through the firmware's own copy, and a program that asks for a code
 * page gets one. emu_core builds the same tables at runtime with font.c, so
 * the store is compared against those rather than against the generator that
 * produced the asset — a check the two sides cannot both be wrong about.
 *
 * No second machine runs. emu_core is linked for its tables alone, and
 * sys_init is what fills them: font.c declares that storage uninitialized,
 * so without it these compare against whatever was there.
 *
 * The store is only reachable by peeking the fabric because nothing
 * reads it back; the terminal renders from it and that is all. So the
 * faces the terminal never shows — italic, the DEC graphics — are
 * checked here and nowhere else.
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

extern "C"
{
/* The emulator's tables, as reference data. Its header carries no linkage
 * guard of its own; every other consumer is C. */
#include "core/sys/sys.h"
}
#include "tb_asm.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

extern "C" {
#include "core/term/font.h"
}

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vwiring *dut;

/* A program that asks for one code page and stops: the two API
 * registers, the operation, the trampoline, and the answer to the
 * console so the bench reads the page the program was given. */
static std::vector<uint8_t> rom_code_page(uint16_t cp)
{
    tb_asm a;
    a.store(TB_API_A, (uint8_t)cp);
    a.store(TB_API_X, (uint8_t)(cp >> 8));
    a.call(0x03);
    a.put_ax();
    a.stp();
    return tb_rom_image(TB_ORG, a.b);
}

/* The path the libraries actually take: code_page() is ria_attr_set
 * followed by ria_attr_get. The value is a 32-bit xstack push, most
 * significant byte first so the least significant is the one on top,
 * which is what api_pop_uint32_end reads. Every answer goes to the
 * console, A then X, so the bench reads exactly what the program read. */
static std::vector<uint8_t> rom_attr_code_page(const std::vector<uint16_t> &sets)
{
    const uint8_t ATTR_CODE_PAGE = 0x02;
    tb_asm a;
    for (uint16_t cp : sets)
    {
        a.pushl(cp);
        a.call_a(0x0B, ATTR_CODE_PAGE);
        a.put_ax();
    }
    a.call_a(0x0A, ATTR_CODE_PAGE);
    a.put_ax();
    a.stp();
    return tb_rom_image(TB_ORG, a.b);
}

/* The store answers a word at a time; the faces are word arrays in the
 * fabric, so a byte is a lane of one. */
template <typename Face>
static uint8_t face_byte(Face &face, size_t at)
{
    return (uint8_t)(face[at / 4] >> (8 * (at % 4)));
}

/* The page a program was handed back, as its two console bytes. */
static uint16_t reported(const std::string &out, size_t at)
{
    return (uint16_t)((uint8_t)out[at] | ((uint8_t)out[at + 1] << 8));
}

UTEST(fontstore, boot_image_matches_font_init)
{
    ASSERT_TRUE(tb_boot(dut, rom_code_page(437)));
    font_init();
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f8, i), font8[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__ital, i),
                  italic16[i]);
    for (size_t i = 0; i < 512; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__dec, i),
                  font_dec_16[i]);
}

/* Every page font.c accepts, against the tables font.c itself builds for
 * it. The low halves must survive the change untouched — a page rewrites
 * the high half alone, and writing the whole face would be the easy way
 * to pass this while doing four times the work. */
static void one_page(int *utest_result, uint16_t cp)
{
    std::string out;
    ASSERT_TRUE(tb_boot(dut, rom_code_page(cp), &out));
    ASSERT_EQ(out.size(), (size_t)2);
    ASSERT_EQ(reported(out, 0), cp);
    font_init();
    font_set_code_page(cp);
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f8, i), font8[i]);
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
    ASSERT_TRUE(tb_boot(dut, rom_code_page(999), &out));
    ASSERT_EQ(out.size(), (size_t)2);
    ASSERT_EQ(reported(out, 0), (uint16_t)437);
    font_init();
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f8, i), font8[i]);
}

/* The bug this all came from: a program that asks the attribute API what
 * page it is on was told -1, and skipped its CP437 path on a machine
 * that was already on 437. */
UTEST(fontstore, attribute_get_reports_the_boot_default)
{
    std::string out;
    ASSERT_TRUE(tb_boot(dut, rom_attr_code_page({}), &out));
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
    ASSERT_TRUE(tb_boot(dut, rom_attr_code_page({850, 1252}), &out));
    ASSERT_EQ(out.size(), (size_t)6);
    ASSERT_EQ(reported(out, 0), (uint16_t)0); /* both sets succeeded */
    ASSERT_EQ(reported(out, 2), (uint16_t)0);
    ASSERT_EQ(reported(out, 4), (uint16_t)850);
    font_init();
    font_set_code_page(850);
    auto *r = dut->rootp;
    for (size_t i = 0; i < 4096; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f16, i), font16[i]);
    for (size_t i = 0; i < 2048; i++)
        ASSERT_EQ(face_byte(r->wiring__DOT__font__DOT__f8, i), font8[i]);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    sys_init(); /* font_init, which lays the reference tables out */
    dut = new Vwiring;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
