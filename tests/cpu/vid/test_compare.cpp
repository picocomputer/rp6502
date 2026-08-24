/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The syscall ABI, on whichever machine this tree built. One hand-assembled
 * image writes the RIA's UART register directly, pushes an xstack argument
 * through std write (op $18, fd 1 in AX), reads back the byte count, and asks
 * for an op nobody implements ($35) to see its A/X/errno.
 *
 * What reaches the terminal is the whole claim, and it is written down below
 * rather than derived by running the emulator inside the test — which is what
 * this did, comparing the fabric's stream against the emulator's by sliding
 * one along the other. Both machines answer the same bytes now.
 */

#include "mut.h"
#include "tb_rom.h"
#include "utest.h"

#include <stdio.h>
#include <stdlib.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

/* UART 'A'; push "BC" and write it to stdout via op $18 (fd 1 in AX);
 * print the returned count; op $35 for ENOSYS; print AX and the errno
 * cells; STP. Every byte lands on the console in trampoline order. */
static const uint8_t prog[] = {
    0xA9, 0x41,       /* lda #'A'      */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xA9, 0x43,       /* lda #'C'      */
    0x8D, 0xEC, 0xFF, /* sta $FFEC     */
    0xA9, 0x42,       /* lda #'B'      */
    0x8D, 0xEC, 0xFF, /* sta $FFEC     */
    0xA9, 0x01,       /* lda #1        */
    0x8D, 0xF4, 0xFF, /* sta $FFF4     */
    0xA9, 0x00,       /* lda #0        */
    0x8D, 0xF6, 0xFF, /* sta $FFF6     */
    0xA9, 0x18,       /* lda #$18      */
    0x8D, 0xEF, 0xFF, /* sta $FFEF     */
    0x20, 0xF1, 0xFF, /* jsr $FFF1     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xA9, 0x35,       /* lda #$35      */
    0x8D, 0xEF, 0xFF, /* sta $FFEF     */
    0x20, 0xF1, 0xFF, /* jsr $FFF1     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0x8E, 0xE1, 0xFF, /* stx $FFE1     */
    0xAD, 0xED, 0xFF, /* lda $FFED     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xAD, 0xEE, 0xFF, /* lda $FFEE     */
    0x8D, 0xE1, 0xFF, /* sta $FFE1     */
    0xDB,             /* stp           */
};
static const uint8_t vectors[] = {0x00, 0x03};

UTEST(compare, syscall_abi_console_stream)
{
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog, sizeof(prog));
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    /* One image, whichever machine this is. */
    const char *path = TEST_SCRATCH "/test_compare.rp6502";
    ASSERT_TRUE(tb_rom_write(path, rom));

    mut_console_start();
    ASSERT_TRUE(mut_boot(path));

    /* Every byte the program puts on the terminal, in order:
     *   41 'A'   straight out the RIA's UART register
     *   42 43    the xstack write through std op $18 to fd 1; the stack grows
     *   02       down, so 'B' pushed last pops first, and AX comes back 2
     *   FF FF    A and X from op $35, which nobody implements
     *   FF FF    and the errno word it left at $FFED/$FFEE
     * Eight bytes, and the last four being FF is the ENOSYS path saying so
     * four different ways. */
    static const char want[] = {0x41, 0x42, 0x43, 0x02,
                                (char)0xFF, (char)0xFF, (char)0xFF, (char)0xFF};
    size_t len;
    const char *out = mut_console(&len);
    ASSERT_EQ(len, sizeof want);
    ASSERT_EQ(memcmp(out, want, len), 0);
}

MUT_MAIN()