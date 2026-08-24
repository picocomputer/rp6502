/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The two assemblers agree wherever they overlap, which is a claim only
 * this file makes.
 *
 * A program gets built in two places — a generator writing a file, a
 * bench building one in memory — so there is a Python spelling in
 * tests/gen/rp6502_asm.py and a C++ one in tests/bench/tb_asm.h. They are
 * not the same assembler and no longer pretend to be: Python carries the
 * whole instruction set and a symbol table, C++ carries what a bench
 * parameterizes at run time. The same program is written in both here
 * and the encoded bytes have to match.
 *
 * The container is not compared any more. There is one writer of it now
 * — tools/rp6502.py, which the generators package through — and tb_rom.h
 * writes the headerless form on purpose, for the images that writer
 * refuses. Two formats, deliberately, so comparing them proves nothing.
 *
 * What would drift first is push_str. The xstack grows down, so a
 * string goes on backwards, and a spelling that got that the other way
 * round would assemble cleanly and fail only on a machine.
 */

#include "tb_asm.h"
#include "utest.h"

#include <cstdio>

#include <cstdint>
#include <vector>

UTEST_MAIN();

/* asm_ref.py's program, instruction for instruction. */
static tb_asm build()
{
    tb_asm p;

    p.lda(0x12);
    p.ldx(0x34);
    p.ldy(0x56);
    p.lda_abs(0x0200);
    p.ldx_abs(0x0201);
    p.sta(0x0202);
    p.stx(0x0203);
    p.inc(0x0204);

    uint16_t top = p.here();
    p.inx();
    p.dex();
    p.bit(0x0205);
    p.jsr(top);
    p.jmp(top);
    p.rts();

    p.store(0x0206, 0x78);
    p.push(0x9A);
    p.pushw(0xBCDE);
    p.pushl(0x01234567);
    p.push_str("MSC0:\xff");

    p.push_str("T.DAT");
    p.lda(0x02 | 0x10); /* O_WRONLY | O_CREAT */
    p.sta(TB_API_A);
    p.call(0x14); /* OP_OPEN */
    p.call_a(0x15, 0x03); /* OP_CLOSE */
    p.xreg(1, 0, 0, 0x0001);
    /* xreg takes one word in this spelling; the reference program's
     * three-word call is the same three pushes. */
    p.push(1);
    p.push(0);
    p.push(1);
    p.pushw(0x0003);
    p.pushw(0x0000);
    p.pushw(0x1000);
    p.call(0x01);

    p.poke(0x8000, 0x28);
    p.lda(0x07);
    p.sta(TB_RIA_TX);
    p.putc_a();

    p.raw({0xEA, 0xEA});
    p.stp();
    p.raw("asm", 4);
    return p;
}

UTEST(asm, the_two_spellings_assemble_the_same_program)
{
    FILE *f = fopen(ASM_REF, "rb");
    ASSERT_TRUE(f != NULL);
    std::vector<uint8_t> want;
    uint8_t buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0)
        want.insert(want.end(), buf, buf + n);
    fclose(f);
    ASSERT_FALSE(want.empty());

    std::vector<uint8_t> got = build().b;

    /* The offset of the first difference, not merely that there is
     * one: the two spellings are read side by side, and a byte number
     * says which instruction. */
    size_t at = 0;
    while (at < got.size() && at < want.size() && got[at] == want[at])
        at++;
    ASSERT_EQ(at, want.size());
    ASSERT_EQ(got.size(), want.size());
}
