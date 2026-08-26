/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The op 0x01 and attribute probe program, as an image, written once.
 *
 * Thirty-one calls, each printing what it returned and the errno it left:
 * the xreg error paths, a canvas switch, a full mode 3 program, the sprite
 * slots, the PSG and OPL pointers through the soft CPU's validation, and the
 * ATR set including the bell mute.
 *
 * Two suites boot it. tests/cpu/ria holds the console stream it prints to the
 * bytes written down there, on whichever machine that tree built;
 * tests/rtl/ria asks what the same program left in the fabric's registers,
 * which is a question only that machine can answer. Written here so the two
 * cannot come to be booting different programs.
 */

#ifndef _TESTS_CPU_RIA_XREG_PROG_H_
#define _TESTS_CPU_RIA_XREG_PROG_H_

#include "tb_asm.h"
#include "tb_rom.h"

#include <vector>

static void xreg_rom(std::vector<uint8_t> &rom)
{
    tb_asm p;
    auto push = [&](uint8_t v) { p.push(v); };
    auto pushw = [&](uint16_t w) { p.pushw(w); };
    /* Four bytes a result: what the call returned, then the errno it
     * left, so a difference anywhere in the dispatch shows up as a byte. */
    auto op1 = [&]() {
        p.call(0x01);
        p.put_ax();
        p.put_errno();
    };
    auto opn = [&](uint8_t op, uint8_t a) {
        p.call_a(op, a);
        p.put_ax();
        p.put_errno();
    };
    auto opn_errno = [&](uint8_t op, uint8_t a) {
        p.call_a(op, a);
        p.put_errno();
    };

    /* ATTR_ERRNO_OPT first, because until a program picks a map every
     * errno reads back as -1 — which is also what api_run left in the
     * register, so nothing below could tell a refusal from a success
     * without this. */
    push(0); push(0); push(0); push(1); /* cc65 */
    opn(0x0B, 0);

    /* ATTR_CLK_RUN_*, before anything that fails: errno is sticky, so
     * only here is 0xFFFF still the untouched value and an absent case
     * still visible. The run clock itself is not compared; it is a
     * different number of microseconds on a simulated machine than on
     * an emulated one, and comparing it would be comparing the benches
     * rather than the dispatch. */
    opn_errno(0x0A, 0x10);
    opn_errno(0x0A, 0x11);
    opn_errno(0x0A, 0x12);
    opn_errno(0x0A, 0x13);

    /* VGA control channel: RIA-private, EACCES. */
    push(1); push(15); push(0); pushw(0);
    op1();
    /* Device 8: EINVAL. */
    push(8); push(0); push(0); pushw(0);
    op1();
    /* Misaligned stack: an extra byte, EINVAL. */
    push(1); push(0); push(0); pushw(0); push(0xAA);
    op1();
    /* Canvas 1, 320x240. */
    push(1); push(0); push(0); pushw(1);
    op1();
    /* Mode 3, 1bpp, config at $1000, plane 0, whole canvas. */
    push(1); push(0); push(1);
    pushw(3); pushw(0); pushw(0x1000); pushw(0); pushw(0); pushw(0);
    op1();
    /* Mode 4 plain sprites: 3 descriptors at $2000 on plane 1. */
    push(1); push(0); push(1);
    pushw(4); pushw(0); pushw(0x2000); pushw(3); pushw(1);
    pushw(0); pushw(0);
    op1();
    /* Mode 4 attribute 2: no such engine, EINVAL. */
    push(1); push(0); push(1);
    pushw(4); pushw(2); pushw(0x2000); pushw(3); pushw(1);
    pushw(0); pushw(0);
    op1();
    /* Mode 5 4bpp 16x16: 2 descriptors at $3000 on plane 2. */
    push(1); push(0); push(1);
    pushw(5); pushw(10); pushw(0x3000); pushw(2); pushw(2);
    pushw(0); pushw(0);
    op1();

    /* The PSG pointer through the soft CPU's validation: an accept,
     * the three rejects, then a working pointer left standing. */
    push(0); push(1); push(0); pushw(0x9000);
    op1();
    push(0); push(1); push(0); pushw(0x9001); /* odd */
    op1();
    push(0); push(1); push(0); pushw(0xFFC2); /* over the bound */
    op1();
    push(0); push(1); push(0); pushw(0x90C2); /* crosses its page */
    op1();
    push(0); push(1); push(0); pushw(0x8000);
    op1();

    /* The OPL pointer, whose validation is only that the block is page
     * aligned: a reject, then one that stands. */
    push(0); push(1); push(1); pushw(0xF001); /* not a page */
    op1();
    push(0); push(1); push(1); pushw(0xF000);
    op1();

    /* ATTR_BEL: read the default, mute, ring silently, reject a bad
     * value, unmute, ring for real. */
    opn(0x0A, 5);
    push(0); push(0); push(0); push(0);
    opn(0x0B, 5);
    opn(0x0A, 5);
    p.store(TB_RIA_TX, 0x07); /* BEL, muted */
    push(0); push(0); push(0); push(2); /* out of range */
    opn(0x0B, 5);
    push(0); push(0); push(0); push(1);
    opn(0x0B, 5);
    p.store(TB_RIA_TX, 0x07); /* BEL, rings */

    /* ATTR_CODE_PAGE: a page both machines carry, one neither does — a
     * no-op that still answers success — and back to 437. Relative
     * moves only, so this does not also assert that two machines boot
     * the same locale. The soft CPU's page list is the font asset's and
     * the RIA's is f_setcp's; this is what says they agree. */
    push(0); push(0); push(3); push(0x52); /* 850 */
    opn(0x0B, 2);
    opn(0x0A, 2);
    push(0); push(0); push(4); push(0xE4); /* 1252 */
    opn(0x0B, 2);
    opn(0x0A, 2);
    push(0); push(0); push(1); push(0xB5); /* 437 */
    opn(0x0B, 2);
    opn(0x0A, 2);

    /* ATTR_EXIT_CODE and ATTR_SIGINT, which are reads a program makes about
     * the run it is in: nothing has exited and nothing has interrupted, so
     * both answer zero on a machine that keeps them at all. They are here
     * because one machine used to answer EINVAL to the pair -- its attribute
     * table was a copy that never grew the two entries. */
    opn(0x0A, 7);
    opn(0x0A, 8);
    p.stp();

    rom = tb_rom_image(TB_ORG, p.b);
}

#endif /* _TESTS_CPU_RIA_XREG_PROG_H_ */
