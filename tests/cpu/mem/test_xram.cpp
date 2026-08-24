/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * XRAM records through the staged loader, on whichever machine this tree
 * built.
 *
 * The rule is one line: a record below 0x10000 is RAM, at or above it is
 * XRAM, and one that crosses the boundary or runs off the top of XRAM is not
 * a record at all. What that rule is worth is where the bytes land, so the
 * accepted image is compared against the whole 64K written out here — every
 * byte of it, including the top one and every byte no record named.
 *
 * A refusal has to be asked what it left behind and not merely whether the
 * machine came up: the straddling record here is the one that would overwrite
 * the reset vector if it were taken, so a loader that took it hangs, and a
 * case that read only the boot's verdict would call that hang a refusal.
 */

#include "mut.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstring>
#include <vector>

static const uint8_t prog_stp[] = {0xDB};
static const uint8_t vectors[] = {0x00, 0x03};

/* Enough of a program to reach STP, so an accepted image is one that ran. */
static std::vector<uint8_t> rom_shell()
{
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog_stp, sizeof(prog_stp));
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return rom;
}

UTEST(xram, records_land_where_the_rule_says)
{
    /* Patterns on both sides of the boundary, one ending on the last byte. */
    uint8_t pat1[256], pat2[255];
    for (int i = 0; i < 256; i++)
        pat1[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 255; i++)
        pat2[i] = (uint8_t)(0xA5 ^ i);

    std::vector<uint8_t> rom = rom_shell();
    tb_rom_record(rom, 0x10040, pat1, sizeof(pat1));
    tb_rom_record(rom, 0x1FF01, pat2, sizeof(pat2));
    const char *path = TEST_SCRATCH "/test_xram.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));
    ASSERT_TRUE(mut_boot(path));

    static uint8_t want[0x10000], got[0x10000];
    memset(want, 0, sizeof want);
    memcpy(want + 0x0040, pat1, sizeof pat1);
    memcpy(want + 0xFF01, pat2, sizeof pat2);
    mut_xram(0, got, sizeof got);
    ASSERT_EQ(memcmp(got, want, sizeof want), 0);
}

/* Every byte distinct from an untouched one, so the half of a taken record
 * that would reach XRAM is visible there. */
static void junk_bytes(uint8_t *at, size_t len)
{
    for (size_t i = 0; i < len; i++)
        at[i] = (uint8_t)(0xA5 ^ i);
}

/* A boot is a fresh machine on both, so XRAM a refusal did not write is the
 * zero it came up as. */
static void xram_untouched(int *utest_result, uint32_t at, size_t len)
{
    uint8_t got[0x20];
    mut_xram(at, got, len);
    for (size_t i = 0; i < len; i++)
        ASSERT_EQ(got[i], 0);
}

UTEST(xram, a_record_across_the_boundary_is_refused)
{
    uint8_t junk[0x20];
    junk_bytes(junk, sizeof(junk));
    std::vector<uint8_t> rom = rom_shell();
    tb_rom_record(rom, 0xFFF0, junk, sizeof(junk));
    const char *path = TEST_SCRATCH "/test_xram_straddle.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));
    ASSERT_FALSE(mut_boot(path));
    xram_untouched(utest_result, 0x0000, 0x10);
}

UTEST(xram, a_record_off_the_top_is_refused)
{
    uint8_t junk[0x20];
    junk_bytes(junk, sizeof(junk));
    std::vector<uint8_t> rom = rom_shell();
    tb_rom_record(rom, 0x1FFF0, junk, sizeof(junk));
    const char *path = TEST_SCRATCH "/test_xram_overrun.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));
    ASSERT_FALSE(mut_boot(path));
    xram_untouched(utest_result, 0xFFF0, 0x10);
}

MUT_MAIN()
