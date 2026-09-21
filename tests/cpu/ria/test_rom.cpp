/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "Vwiring.h"
#include "Vwiring___024root.h"

#include "tb_machine.h"
#include "tb_rom.h"
#include "utest.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static Vwiring *dut;

static std::vector<uint8_t> make_rom()
{
    static const uint8_t prog[] = {
        0xA2, 0x00,       /*       ldx #0     */
        0xBD, 0x14, 0x03, /* loop: lda msg,x  */
        0xF0, 0x0C,       /*       beq done   */
        0x2C, 0xE0, 0xFF, /* wait: bit $FFE0  */
        0x10, 0xFB,       /*       bpl wait   */
        0x8D, 0xE1, 0xFF, /*       sta $FFE1  */
        0xE8,             /*       inx        */
        0xD0, 0xF0,       /*       bne loop   */
        0xEA,             /*       nop        */
        0xDB,             /* done: stp        */
        'R', 'P', 0,      /* msg at $0314     */
    };
    static const uint8_t vectors[] = {0x00, 0x03};
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    tb_rom_record(rom, 0x0300, prog, sizeof(prog));
    tb_rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return rom;
}

static void run_staged(int *utest_result, bool slot_by_port,
                       int stall_cycles)
{
    std::vector<uint8_t> rom = make_rom();

    tb_reset(dut);
    if (slot_by_port)
    {
        dut->slot_len = (uint32_t)rom.size();
        dut->slot_set = 1;
        tb_clock(dut);
        dut->slot_set = 0;
    }
    else
        dut->rootp->wiring__DOT__soc__DOT__mmio_slot_len =
            (uint32_t)rom.size();

    std::string rv_out, cpu_out;
    int stalled = 0;
    bool quiet = tb_quiet(dut, [&] {
        uint32_t a = dut->wiring_stage_addr;
        if (stall_cycles && dut->wiring_stage_pend)
        {
            if (stalled < stall_cycles)
            {
                dut->stage_stall = 1;
                stalled++;
            }
            else
            {
                dut->stage_stall = 0;
                tb_host_tick(dut, rom);
                dut->stage_half = tb_stage_half(rom, a);
            }
        }
        else
        {
            dut->stage_stall = 0;
            tb_host_tick(dut, rom);
            dut->stage_half = tb_stage_half(rom, a);
            stalled = 0;
        }
        tb_clock(dut);
        if (!dut->wiring_stage_pend)
            stalled = 0;
        if (dut->wiring_rv_tx_valid)
            rv_out.push_back((char)dut->wiring_rv_tx_data);
        if (dut->wiring_tx_valid)
            cpu_out.push_back((char)dut->wiring_tx_data);
    });

    if (getenv("BOOT_DEBUG"))
        fprintf(stderr, "cpu_out=[%s]\nrv_out=[%s]\nquiet=%d\n",
                cpu_out.c_str(), rv_out.c_str(), (int)quiet);
    ASSERT_TRUE(quiet);
    ASSERT_STREQ(cpu_out.c_str(), "RP");
}

UTEST(rom, staged_rom_boots)
{
    ASSERT_TRUE(tb_firmware(dut, SW_BIN));
    run_staged(utest_result, false, 0);
}

UTEST(rom, slot_posted_by_port)
{
    ASSERT_TRUE(tb_firmware(dut, SW_BIN));
    run_staged(utest_result, true, 0);
}

UTEST(rom, staging_stalls_like_sdram)
{
    ASSERT_TRUE(tb_firmware(dut, SW_BIN));
    run_staged(utest_result, true, 12);
}

UTEST_STATE();

int main(int argc, const char *const argv[])
{
    Verilated::commandArgs(argc, const_cast<char **>(argv));
    dut = new Vwiring;
    int rc = utest_main(argc, argv);
    dut->final();
    delete dut;
    return rc;
}
