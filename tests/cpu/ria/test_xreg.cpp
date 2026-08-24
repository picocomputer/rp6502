/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * Op 0x01 plumbing without pixels: the error paths — misaligned stack, bad
 * device, the RIA-private VGA control channel — then a canvas switch and a
 * full mode 3 program, byte-exact on the console against the oracle and
 * structurally against the RTL's canvas and scanline program. The PSG
 * pointer runs the soft CPU's validation both ways, and ATR_BEL mutes
 * and unmutes the teletype bell around two rung BELs — the strike must
 * land exactly once.
 *
 * The attribute API rides along, because the same console comparison
 * makes the RIA's own atr.c the judge of the soft CPU's answers:
 * ATR_CODE_PAGE moves to a page both machines carry and then to one
 * neither does, and the four ATR_CLK_RUN_* reads say their cases exist
 * without saying what time it is.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_asm.h"
#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

UTEST(xreg, dispatch_matches_the_oracle)
{
    tb_asm p;
    auto push = [&](uint8_t v) { p.push(v); };
    auto pushw = [&](uint16_t w) { p.pushw(w); };
    /* Four bytes a result: what the call returned, then the errno it
     * left. The oracle runs the same program, so a difference anywhere
     * in the dispatch shows up as a byte. */
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

    /* ATR_ERRNO_OPT first, because until a program picks a map every
     * errno reads back as -1 — which is also what api_run left in the
     * register, so nothing below could tell a refusal from a success
     * without this. */
    push(0); push(0); push(0); push(1); /* cc65 */
    opn(0x0B, 0);

    /* ATR_CLK_RUN_*, before anything that fails: errno is sticky, so
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

    /* ATR_BEL: read the default, mute, ring silently, reject a bad
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

    /* ATR_CODE_PAGE: a page both machines carry, one neither does — a
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
    p.stp();

    std::vector<uint8_t> rom = tb_rom_image(TB_ORG, p.b);
    ASSERT_TRUE(tb_rom_write(TEST_SCRATCH "/test_xreg.rp6502", rom));
    oracle_init();
    oracle_tap_start();
    ASSERT_TRUE(oracle_restart(TEST_SCRATCH "/test_xreg.rp6502"));
    oracle_run_frames(30);
    int ow = 0, oh = 0;
    oracle_canvas_size(&ow, &oh);
    ASSERT_EQ(ow, 320);
    ASSERT_EQ(oh, 240);
    size_t tap_len;
    const char *tap = oracle_tap_data(&tap_len);
    std::string oracle_out(tap, tap_len);

    std::string cpu_out;
    int strikes = 0;
    bool bel_gate_prev = false;
    ASSERT_TRUE(tb_boot_each(dut, rom, &cpu_out, [&] {
        /* The bell is a voice of the PSG now and the soft CPU rings it by
         * writing the voice's gate, so a strike is that bit going up. */
        const bool bg =
            (dut->rootp->rp6502__DOT__aud_psg__DOT__bel_hi >> 16) & 1;
        if (!bel_gate_prev && bg)
            strikes++;
        bel_gate_prev = bg;
    }));

    /* Twenty-seven results at four bytes each, four errno-only ones at
     * two, plus the two BEL characters, identical on both machines. */
    ASSERT_EQ(cpu_out.size(), (size_t)(27 * 4 + 4 * 2 + 2));

    /* The muted BEL never struck; the unmuted one did. */
    ASSERT_EQ(strikes, 1);

    /* The PSG took 0x8000 and the OPL took 0xF000 after it. Setting up
     * either engine resets the other, so the PSG's pointer is parked. */
    ASSERT_EQ(dut->rootp->rp6502__DOT__aud_psg__DOT__xaddr, 0xFFFF);
    /* And the OPL's, which is the whole path the device depends on:
     * the xreg dispatch, the soft CPU's validation, the MMIO write and
     * the machine's decode of it. */
    ASSERT_TRUE(dut->rootp->rp6502__DOT__aud_opl__DOT__enabled);
    ASSERT_EQ(dut->rootp->rp6502__DOT__aud_opl__DOT__page, 0xF0);
    ASSERT_TRUE(oracle_out.size() >= cpu_out.size());
    ASSERT_EQ(memcmp(oracle_out.data() + oracle_out.size() - cpu_out.size(),
                     cpu_out.data(), cpu_out.size()), 0);

    /* The RTL landed it: canvas 1, mode 3 entries across [0, 240) on
     * plane 0 with the config pointer, nothing at 240. */
    auto *r = dut->rootp;
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__canvas_shadow, 1);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_e[0],
              0x80000000u | (3u << 16));
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_c[0], 0x1000);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_e[239 * 4],
              0x80000000u | (3u << 16));
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__fill_e[240 * 4], 0u);

    /* The sprite slots: mode 4 plane 1, mode 5 plane 2, count over
     * config in the second word. spr_e keeps only the live bits —
     * {enable, mode[2:0], attr[15:0]} — because the twelve dead ones
     * cost three M10K to store. */
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_e[100 * 4 + 1],
              (1u << 19) | (4u << 16));
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_c[100 * 4 + 1],
              (3u << 16) | 0x2000u);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_e[100 * 4 + 2],
              (1u << 19) | (5u << 16) | 10u);
    ASSERT_EQ(r->rp6502__DOT__vid_prog__DOT__spr_c[100 * 4 + 2],
              (2u << 16) | 0x3000u);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vrp6502;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
