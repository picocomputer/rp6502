/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Op 0x01 and the attribute API, on whichever machine this tree built.
 *
 * Thirty-one probes, each printing what the call returned and the errno it
 * left: the error paths — misaligned stack, bad device, the RIA-private VGA
 * control channel — a canvas switch and a full mode 3 program, the sprite
 * slots, the PSG and OPL pointers through the soft CPU's validation both
 * ways, and the ATR set, ATTR_BEL among it.
 *
 * The stream those probes print is the expectation, byte for byte, and an
 * array rather than a checksum on purpose: the whole value of the suite is
 * which errno each probe answered with, and a checksum would only say that
 * something moved.
 *
 * It used to say the same thing by running the emulator inside the test and
 * sliding one console along the other. What that could never carry is what
 * the program left in the fabric — the bell that struck once, the pointers
 * the audio devices took, the scanline program — and that is in
 * tests/rtl/ria, against the machine that has those registers.
 */

#include "mut.h"
#include "utest.h"
#include "xreg_prog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

UTEST(xreg, dispatch_and_attributes)
{
    std::vector<uint8_t> rom;
    xreg_rom(rom);
    const char *path = TEST_SCRATCH "/test_xreg.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));

    mut_console_start();
    ASSERT_TRUE(mut_boot(path));

    /* Twenty-nine results at four bytes each, four errno-only ones at two,
     * plus the two BEL characters. Set RP6502_BLESS_CRC to have a run print
     * the stream in the form it is pasted back as. */
    static const uint8_t want[] = {
        0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
        0xFF, 0xFF, 0x03, 0x00, 0xFF, 0xFF, 0x07, 0x00, 0xFF, 0xFF, 0x07, 0x00,
        0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00,
        0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00,
        0xFF, 0xFF, 0x07, 0x00, 0xFF, 0xFF, 0x07, 0x00, 0xFF, 0xFF, 0x07, 0x00,
        0x00, 0x00, 0x07, 0x00, 0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00,
        0x01, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00,
        0x07, 0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x07, 0x00, 0x00,
        0x07, 0x00, 0x52, 0x03, 0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0x52, 0x03,
        0x07, 0x00, 0x00, 0x00, 0x07, 0x00, 0xB5, 0x01, 0x07, 0x00, 0x00, 0x00,
        0x07, 0x00, 0x00, 0x00, 0x07, 0x00,
    };
    size_t len;
    const char *out = mut_console(&len);
    if (getenv("RP6502_BLESS_CRC"))
        for (size_t i = 0; i < len; i++)
            fprintf(stderr, i % 12 == 0 ? "\n        0x%02X," : " 0x%02X,",
                    (unsigned char)out[i]);
    ASSERT_EQ(len, sizeof want);
    ASSERT_EQ(memcmp(out, want, sizeof want), 0);
}

MUT_MAIN()
