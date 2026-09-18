/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

extern "C"
{
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

template <typename Face>
static uint8_t face_byte(Face &face, size_t at)
{
    return (uint8_t)(face[at / 4] >> (8 * (at % 4)));
}

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

UTEST(fontstore, attribute_get_reports_the_boot_default)
{
    std::string out;
    ASSERT_TRUE(tb_boot(dut, rom_attr_code_page({}), &out));
    ASSERT_EQ(out.size(), (size_t)2);
    ASSERT_EQ(reported(out, 0), (uint16_t)437);
}

/* The font asset has no code page 1252, so the second set leaves 850 in
 * force. */
UTEST(fontstore, attribute_set_takes_a_page_and_ignores_the_rest)
{
    std::string out;
    ASSERT_TRUE(tb_boot(dut, rom_attr_code_page({850, 1252}), &out));
    ASSERT_EQ(out.size(), (size_t)6);
    ASSERT_EQ(reported(out, 0), (uint16_t)0);
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
    sys_init();
    dut = new Vwiring;
    int rc = utest_main(argc, argv);
    delete dut;
    return rc;
}
