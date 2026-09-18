/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * tests/gen/time_rom_gen.py writes the program, which leaves the length
 * strftime returned at XRAM $0000 and the formatted text after it.
 */

#include "mut.h"
#include "utest.h"

#include <cstring>

UTEST(time, gmtime_formats_a_fixed_instant)
{
    ASSERT_TRUE(mut_boot(TIME_ROM));
    static const char want[] = "2025-01-01 12:00:00";
    uint8_t got[sizeof want];
    mut_xram(0, got, sizeof got);
    ASSERT_EQ(got[0], (uint8_t)(sizeof want - 1));
    ASSERT_EQ(memcmp(got + 1, want, sizeof want - 1), 0);
}

MUT_MAIN()
