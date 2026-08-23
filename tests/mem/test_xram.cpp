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
#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include "host/pico/ria/sys/mem.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vrp6502 *dut;

/* Boot the staged image; true when the firmware accepted and halted 0. */
static bool rtl_boot(const std::vector<uint8_t> &rom, std::string *rv_out)
{
    /* RESB is the OS's line and takes no reset, so each boot is a fresh
     * machine rather than a pulse. */
    if (dut)
    {
        dut->final();
        delete dut;
    }
    dut = new Vrp6502;
    dut->clk_sys = 0;
    dut->clk_rv = 0;
    dut->eval();
    if (!tb_firmware(dut, SW_BIN))
        return false;
    tb_reset(dut);
    dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len = (uint32_t)rom.size();
    rv_out->clear();
    /* A rejected image leaves the 6502 in reset; the firmware says so
     * and keeps serving the console. Booted means the program ran. */
    bool ever_ran = false;
    bool quiet = tb_quiet(dut, [&] {
        uint32_t a = dut->rp6502_stage_addr;
        tb_host_tick(dut, rom);
        dut->stage_rdata = tb_stage(rom, a);
        tb_clock(dut);
        if (dut->rootp->rp6502__DOT__resb)
            ever_ran = true;
        if (dut->rp6502_rv_tx_valid)
            rv_out->push_back((char)dut->rp6502_rv_tx_data);
    });
    return quiet && ever_ran;
}

static const uint8_t prog_stp[] = {0xDB};
static const uint8_t vectors[] = {0x00, 0x03};

static std::vector<uint8_t> rom_shell()
{
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog_stp, sizeof(prog_stp));
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
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
    tb_rom_record(rom, 0x10040, pat1, sizeof(pat1));
    tb_rom_record(rom, 0x1FF01, pat2, sizeof(pat2));

    write_rom(TEST_SCRATCH "/test_xram.rp6502", rom);
    oracle_init();
    ASSERT_TRUE(oracle_restart(TEST_SCRATCH "/test_xram.rp6502"));

    std::string rv_out;
    ASSERT_TRUE(rtl_boot(rom, &rv_out));

    /* Every XRAM byte identical, LE words on the RTL side. */
    auto *r = dut->rootp;
    int diffs = 0;
    for (uint32_t a = 0; a < 0x10000; a++)
    {
        size_t wi = a >> 2;
        uint8_t rtl = (a & 3) == 0 ? r->rp6502__DOT__xram__DOT__mem0[wi]
            : (a & 3) == 1 ? r->rp6502__DOT__xram__DOT__mem1[wi]
            : (a & 3) == 2 ? r->rp6502__DOT__xram__DOT__mem2[wi]
                           : r->rp6502__DOT__xram__DOT__mem3[wi];
        if (rtl != xram[a])
            diffs++;
    }
    ASSERT_EQ(diffs, 0);
}

UTEST(xram, straddle_and_overrun_reject_like_the_oracle)
{
    static const uint8_t junk[0x20] = {1, 2, 3};

    std::vector<uint8_t> straddle = rom_shell();
    tb_rom_record(straddle, 0xFFF0, junk, sizeof(junk));
    write_rom(TEST_SCRATCH "/test_xram_straddle.rp6502", straddle);
    ASSERT_FALSE(oracle_restart(TEST_SCRATCH "/test_xram_straddle.rp6502"));
    std::string rv_out;
    ASSERT_FALSE(rtl_boot(straddle, &rv_out));
    ASSERT_TRUE(strstr(rv_out.c_str(), "rom: bad image") != NULL);

    std::vector<uint8_t> overrun = rom_shell();
    tb_rom_record(overrun, 0x1FFF0, junk, sizeof(junk));
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
