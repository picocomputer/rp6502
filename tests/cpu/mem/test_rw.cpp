/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RW0/RW1 on whichever machine this tree built: the data ports read and write
 * XRAM at their address registers, post-incrementing by the signed step. One
 * generated 6502 program walks steps +1, -1, 0, +127; 16-bit wraparound both
 * ways; read-then-step ordering; and the post-api_run defaults (ADDR 0,
 * STEP 1) — printing every byte it reads back.
 *
 * What it prints is the expectation, written down below. It used to be
 * written down AND compared against the emulator running inside the test,
 * which made the emulator the standard rather than the string; now both
 * machines answer the string and neither needs the other present.
 *
 * The whole stream is the expectation, not a tail of it. Both machines put
 * these thirteen bytes on the terminal and nothing else: the OS is silent
 * while a program runs, so a byte that should not be there fails here.
 */

#include "mut.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

UTEST(rw, steps_wraps_and_defaults)
{
    /* A tiny assembler beats hand-counted offsets. */
    std::vector<uint8_t> p;
    auto lda = [&](uint8_t v) { p.insert(p.end(), {0xA9, v}); };
    auto sta = [&](uint16_t a) {
        p.insert(p.end(), {0x8D, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto ldaa = [&](uint16_t a) {
        p.insert(p.end(), {0xAD, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto set = [&](uint16_t reg, uint8_t v) { lda(v); sta(reg); };
    auto rw0 = [&](uint8_t v) { set(0xFFE4, v); };
    auto rd1 = [&]() { ldaa(0xFFE8); sta(0xFFE1); };

    /* Post-api_run defaults: ADDR0=0, STEP0=1 — write "ABC" at 0,1,2. */
    rw0('A'); rw0('B'); rw0('C');
    /* Read them back through RW1. */
    set(0xFFEA, 0); set(0xFFEB, 0); set(0xFFE9, 1);
    rd1(); rd1(); rd1();
    /* Negative step writes land descending. */
    set(0xFFE5, 0xFF); set(0xFFE6, 0x05); set(0xFFE7, 0x00);
    rw0('X'); rw0('Y');
    set(0xFFEA, 4); set(0xFFEB, 0); set(0xFFE9, 1);
    rd1(); rd1(); /* Y then X */
    /* Wraparound 0xFFFF -> 0x0000, written and read. */
    set(0xFFE5, 1); set(0xFFE6, 0xFF); set(0xFFE7, 0xFF);
    rw0('W'); rw0('Z'); /* Z lands at 0, over the 'A' */
    set(0xFFEA, 0xFF); set(0xFFEB, 0xFF);
    rd1(); rd1(); /* W then Z */
    /* Step 0 holds the address; the second write overwrites. */
    set(0xFFE5, 0); set(0xFFE6, 8); set(0xFFE7, 0);
    rw0('Q'); rw0('R');
    set(0xFFEA, 8); set(0xFFEB, 0); set(0xFFE9, 0);
    rd1(); rd1(); /* R then R */
    /* Step +127 strides; read back with the same stride. */
    set(0xFFE5, 127); set(0xFFE6, 0x00); set(0xFFE7, 0x01);
    rw0('a'); rw0('b'); rw0('c');
    set(0xFFEA, 0x00); set(0xFFEB, 0x01); set(0xFFE9, 127);
    rd1(); rd1(); rd1();
    /* Read-then-step ordering: reading RW0 at 2 must return 'C' first. */
    set(0xFFE5, 1); set(0xFFE6, 2); set(0xFFE7, 0);
    ldaa(0xFFE4); sta(0xFFE1);
    p.push_back(0xDB); /* stp */

    static const uint8_t vectors[] = {0x00, 0x03};
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, p.data(), p.size());
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    /* One image, whichever machine this is. */
    const char *path = TEST_SCRATCH "/test_rw.rp6502";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);

    mut_console_start();
    ASSERT_TRUE(mut_boot(path));

    static const char want[] = "ABCYXWZRRabcC";
    size_t len;
    const char *out = mut_console(&len);
    ASSERT_EQ(len, sizeof want - 1);
    ASSERT_EQ(memcmp(out, want, len), 0);
}

MUT_MAIN()
