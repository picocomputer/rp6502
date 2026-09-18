/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * tests/gen/vsync_rom_gen.py writes the program, which copies what it recorded
 * to XRAM at the offsets below before it stops.
 */

#include "mut.h"
#include "utest.h"

#include <cstdint>

enum
{
    POLLED = 0,
    TAKEN = 3,
    IN_IRQ = 4,
    DISABLED = 5,
    STAMPS = 6,
    RESULTS = 9,
};

UTEST(vsync, counter_and_interrupt)
{
    ASSERT_TRUE(mut_boot(VSYNC_ROM));
    uint8_t r[RESULTS];
    mut_xram(0, r, sizeof r);

    ASSERT_EQ(r[POLLED + 1], (uint8_t)(r[POLLED] + 1));
    ASSERT_EQ(r[POLLED + 2], (uint8_t)(r[POLLED + 1] + 1));

    /* The program enables the interrupt in the frame of its last poll, when
     * vsync is already pending. The enable write acknowledges that pending
     * bit, so the first interrupt is taken on the next frame. */
    ASSERT_EQ(r[STAMPS], (uint8_t)(r[POLLED + 2] + 1));
    ASSERT_EQ(r[STAMPS + 1], (uint8_t)(r[STAMPS] + 1));
    ASSERT_EQ(r[STAMPS + 2], (uint8_t)(r[STAMPS + 1] + 1));
    ASSERT_EQ(r[IN_IRQ], 0x80);

    ASSERT_EQ(r[TAKEN], 3);
    ASSERT_EQ(r[DISABLED], 0x80);
}

MUT_MAIN()
