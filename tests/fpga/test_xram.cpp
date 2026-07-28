/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * XRAM records through the staged loader: the emulator's record rule
 * verbatim — RAM below 0x10000, XRAM above, never straddling — with the
 * oracle judging every accept and reject, and the accepted image compared
 * byte for byte between the RTL XRAM and the oracle's xram[].
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
#include "utest.h"

#include "ria/sys/mem.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

static bool load_firmware(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    auto &tcm = dut->rootp->rp6502__DOT__rv__DOT__tcm;
    for (size_t i = 0; i < 32768; i++)
        tcm[i] = 0;
    uint8_t buf[4];
    size_t word = 0, n;
    while ((n = fread(buf, 1, 4, f)) > 0 && word < 32768)
    {
        uint32_t v = 0;
        for (size_t i = 0; i < n; i++)
            v |= (uint32_t)buf[i] << (8 * i);
        tcm[word++] = v;
    }
    fclose(f);
    return true;
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

/* Boot the staged image; true when the firmware accepted and halted 0. */
static bool rtl_boot(const std::vector<uint8_t> &rom, std::string *rv_out)
{
    if (!load_firmware(SW_BIN))
        return false;
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
    rv_out->clear();
    for (int i = 0; i < 16000000; i++)
    {
        uint32_t a = dut->rp6502_stage_addr;
        dut->stage_rdata = a < rom.size() ? rom[a] : 0;
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
        if (dut->rp6502_rv_tx_valid)
            rv_out->push_back((char)dut->rp6502_rv_tx_data);
        if (dut->rp6502_rv_halted)
            break;
    }
    return dut->rp6502_rv_halted && dut->rp6502_rv_exit_code == 0;
}

static const uint8_t prog_stp[] = {0xDB};
static const uint8_t vectors[] = {0x00, 0x03};

static std::vector<uint8_t> rom_shell()
{
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, 0x0300, prog_stp, sizeof(prog_stp));
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return rom;
}

static void write_rom(const char *path, const std::vector<uint8_t> &rom)
{
    FILE *f = fopen(path, "wb");
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);
}

UTEST(xram, records_load_and_match_the_oracle)
{
    /* Patterns on both sides of the boundary, one at the very top. */
    uint8_t pat1[256], pat2[255];
    for (int i = 0; i < 256; i++)
        pat1[i] = (uint8_t)(i * 7 + 1);
    for (int i = 0; i < 255; i++)
        pat2[i] = (uint8_t)(0xA5 ^ i);
    std::vector<uint8_t> rom = rom_shell();
    rom_record(rom, 0x10040, pat1, sizeof(pat1));
    rom_record(rom, 0x1FF01, pat2, sizeof(pat2));

    write_rom(TEST_SCRATCH "/test_xram.rp6502", rom);
    oracle_init();
    ASSERT_TRUE(oracle_restart(TEST_SCRATCH "/test_xram.rp6502"));

    std::string rv_out;
    ASSERT_TRUE(rtl_boot(rom, &rv_out));
    ASSERT_TRUE(strstr(rv_out.c_str(), "rom: staged") != NULL);

    /* Every XRAM byte identical, LE words on the RTL side. */
    auto &mem = dut->rootp->rp6502__DOT__xram__DOT__mem;
    int diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++)
    {
        uint8_t rtl = (uint8_t)(mem[a >> 2] >> (8 * (a & 3)));
        if (rtl != xram[a])
            diffs++;
    }
    ASSERT_EQ(diffs, 0);
}

UTEST(xram, straddle_and_overrun_reject_like_the_oracle)
{
    static const uint8_t junk[0x20] = {1, 2, 3};

    std::vector<uint8_t> straddle = rom_shell();
    rom_record(straddle, 0xFFF0, junk, sizeof(junk));
    write_rom(TEST_SCRATCH "/test_xram_straddle.rp6502", straddle);
    ASSERT_FALSE(oracle_restart(TEST_SCRATCH "/test_xram_straddle.rp6502"));
    std::string rv_out;
    ASSERT_FALSE(rtl_boot(straddle, &rv_out));
    ASSERT_TRUE(strstr(rv_out.c_str(), "rom: bad image") != NULL);

    std::vector<uint8_t> overrun = rom_shell();
    rom_record(overrun, 0x1FFF0, junk, sizeof(junk));
    write_rom(TEST_SCRATCH "/test_xram_overrun.rp6502", overrun);
    ASSERT_FALSE(oracle_restart(TEST_SCRATCH "/test_xram_overrun.rp6502"));
    ASSERT_FALSE(rtl_boot(overrun, &rv_out));
    ASSERT_TRUE(strstr(rv_out.c_str(), "rom: bad image") != NULL);
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
