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
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "tb_quiet.h"
#include "tb_stage.h"
#include "tb_tcm.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;
/* Half clk_sys, rising with it: the PLL's shape, not a divider's. */
static bool rv_phase;

static bool load_firmware(const char *path)
{
    auto *r = dut->rootp;
    return tb_load_tcm(r->rp6502__DOT__rv__DOT__tcm0,
                       r->rp6502__DOT__rv__DOT__tcm1,
                       r->rp6502__DOT__rv__DOT__tcm2,
                       r->rp6502__DOT__rv__DOT__tcm3, path);
}

static uint32_t crc32_buf(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    while (n--)
    {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static void rom_record(std::vector<uint8_t> &rom, uint32_t addr,
                       const uint8_t *data, size_t len)
{
    char line[64];
    snprintf(line, sizeof(line), "$%05X $%zX $%08X\n",
             addr, len, crc32_buf(data, len));
    rom.insert(rom.end(), line, line + strlen(line));
    rom.insert(rom.end(), data, data + len);
}

UTEST(xreg, dispatch_matches_the_oracle)
{
    std::vector<uint8_t> p;
    auto lda = [&](uint8_t v) { p.insert(p.end(), {0xA9, v}); };
    auto sta = [&](uint16_t a) {
        p.insert(p.end(), {0x8D, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto ldaa = [&](uint16_t a) {
        p.insert(p.end(), {0xAD, (uint8_t)a, (uint8_t)(a >> 8)});
    };
    auto push = [&](uint8_t v) { lda(v); sta(0xFFEC); };
    auto pushw = [&](uint16_t w) { push((uint8_t)(w >> 8)); push((uint8_t)w); };
    auto op1 = [&]() {
        lda(0x01);
        sta(0xFFEF);
        p.insert(p.end(), {0x20, 0xF1, 0xFF}); /* jsr $FFF1 */
        sta(0xFFE1);                           /* print A */
        p.insert(p.end(), {0x8E, 0xE1, 0xFF}); /* print X */
        ldaa(0xFFED);
        sta(0xFFE1);
        ldaa(0xFFEE);
        sta(0xFFE1);
    };

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
    auto opn = [&](uint8_t op, uint8_t a) {
        lda(a);
        sta(0xFFF4);
        lda(op);
        sta(0xFFEF);
        p.insert(p.end(), {0x20, 0xF1, 0xFF}); /* jsr $FFF1 */
        sta(0xFFE1);
        p.insert(p.end(), {0x8E, 0xE1, 0xFF});
        ldaa(0xFFED);
        sta(0xFFE1);
        ldaa(0xFFEE);
        sta(0xFFE1);
    };
    opn(0x0A, 5);
    push(0); push(0); push(0); push(0);
    opn(0x0B, 5);
    opn(0x0A, 5);
    lda(0x07); sta(0xFFE1); /* BEL, muted */
    push(0); push(0); push(0); push(2); /* out of range */
    opn(0x0B, 5);
    push(0); push(0); push(0); push(1);
    opn(0x0B, 5);
    lda(0x07); sta(0xFFE1); /* BEL, rings */
    p.push_back(0xDB);

    static const uint8_t vectors[] = {0x00, 0x03};
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, 0x0300, p.data(), p.size());
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    FILE *f = fopen(TEST_SCRATCH "/test_xreg.rp6502", "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);
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

    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
        dut->eval();
        dut->clk_rv = 0;
    dut->clk_sys = 0;
        dut->eval();
    }
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    std::string cpu_out;
    int strikes = 0;
    uint8_t bel_count_prev = 0;
    ASSERT_TRUE(tb_quiet(dut, [&] {
        uint32_t a = dut->rp6502_stage_addr;
        dut->stage_rdata = tb_stage(rom, a);
        rv_phase = !rv_phase;
    dut->clk_rv = rv_phase;
    dut->clk_sys = 1;
        dut->eval();
        dut->clk_rv = 0;
    dut->clk_sys = 0;
        dut->eval();
        if (dut->rp6502_tx_valid)
            cpu_out.push_back((char)dut->rp6502_tx_data);
        uint8_t bc = dut->rootp->rp6502__DOT__bel__DOT__count;
        if (bel_count_prev == 0 && bc != 0)
            strikes++;
        bel_count_prev = bc;
    }));

    /* Eighteen results, four bytes each, plus the two BEL characters,
     * identical on both machines. */
    ASSERT_EQ(cpu_out.size(), (size_t)(20 * 4 + 2));

    /* The muted BEL never struck; the unmuted one did. */
    ASSERT_EQ(strikes, 1);

    /* The PSG took 0x8000 and the OPL took 0xF000 after it, and
     * programming either parks the other — the engines are free-running
     * hardware that rp6502.sv sums, so the pointers are what makes them
     * exclusive. The firmware gets this for free from aud_setup handing
     * over the one interrupt; here it is deliberate. */
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
