/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * ffunicode.c is compiled into this test with its three entry points renamed
 * to the ref_ functions declared below, so src/core/str/unicode.c and the
 * file it replaces are compared in one process. The cases make about two
 * million comparisons and take a fraction of a second.
 */

#include "core/str/unicode.h"
#include "oemcp.h"
#include "utest.h"

#include <stdint.h>

uint16_t ref_oem2uni(uint16_t oem, uint16_t cp);
uint16_t ref_uni2oem(uint32_t uni, uint16_t cp);
uint32_t ref_wtoupper(uint32_t uni);

static uint16_t page_at(unsigned i) { return unicode_word(4 + i); }

UTEST_MAIN();

UTEST(unicode, the_image_is_the_one_the_generator_wrote)
{
    ASSERT_TRUE(unicode_init());
    ASSERT_EQ(unicode_word(1), OEMCP_PAGES);
}

UTEST(unicode, every_oem_byte_of_every_page_converts_the_same)
{
    for (unsigned p = 0; p < OEMCP_PAGES; p++)
    {
        uint16_t cp = page_at(p);
        for (unsigned b = 0; b <= 0xFF; b++)
            ASSERT_EQ(ff_oem2uni((uint16_t)b, cp), ref_oem2uni((uint16_t)b, cp));
    }
}

UTEST(unicode, every_code_point_of_every_page_converts_back_the_same)
{
    for (unsigned p = 0; p < OEMCP_PAGES; p++)
    {
        uint16_t cp = page_at(p);
        for (uint32_t u = 0; u <= 0xFFFF; u++)
            ASSERT_EQ(ff_uni2oem(u, cp), ref_uni2oem(u, cp));
    }
}

UTEST(unicode, an_unknown_code_page_converts_to_nothing)
{
    for (uint32_t u = 0x80; u <= 0xFFFF; u += 97)
    {
        ASSERT_EQ(ff_uni2oem(u, 1252), ref_uni2oem(u, 1252));
        ASSERT_EQ(ff_uni2oem(u, 0), ref_uni2oem(u, 0));
    }
    for (unsigned b = 0x80; b <= 0xFF; b++)
        ASSERT_EQ(ff_oem2uni((uint16_t)b, 1252), ref_oem2uni((uint16_t)b, 1252));
}

UTEST(unicode, the_up_case_map_agrees_across_the_whole_of_unicode)
{
    for (uint32_t u = 0; u <= 0x10FFFF; u++)
        ASSERT_EQ(ff_wtoupper(u), ref_wtoupper(u));
}
