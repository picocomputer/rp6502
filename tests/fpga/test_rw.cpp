/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * RW0/RW1 against the oracle: the data ports read and write XRAM at their
 * address registers, post-incrementing by the signed step. One generated
 * 6502 program walks steps +1, -1, 0, +127; 16-bit wraparound both ways;
 * read-then-step ordering; and the post-api_run defaults (ADDR 0, STEP 1)
 * — printing every byte it reads back, compared byte for byte between the
 * machines.
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

UTEST(rw, steps_wraps_and_defaults_match_the_oracle)
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
    rom_record(rom, 0x0300, p.data(), p.size());
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    /* Oracle side, console tapped. */
    FILE *f = fopen(TEST_SCRATCH "/test_rw.rp6502", "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);
    oracle_init();
    oracle_tap_start();
    ASSERT_TRUE(oracle_restart(TEST_SCRATCH "/test_rw.rp6502"));
    oracle_run_frames(30);
    size_t tap_len;
    const char *tap = oracle_tap_data(&tap_len);
    std::string oracle_out(tap, tap_len);

    /* RTL side. */
    ASSERT_TRUE(load_firmware(SW_BIN));
    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
    }
    dut->rst_n = 1;
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();

    std::string cpu_out;
    ASSERT_TRUE(tb_quiet(dut, [&] {
        uint32_t a = dut->rp6502_stage_addr;
        dut->stage_rdata = tb_stage(rom, a);
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
        if (dut->rp6502_tx_valid)
            cpu_out.push_back((char)dut->rp6502_tx_data);
    }));

    ASSERT_STREQ(cpu_out.c_str(), "ABCYXWZRRabcC");
    /* The same bytes reached the oracle's console. */
    ASSERT_TRUE(oracle_out.size() >= cpu_out.size());
    ASSERT_STREQ(oracle_out.substr(oracle_out.size() - cpu_out.size()).c_str(),
                 cpu_out.c_str());
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
