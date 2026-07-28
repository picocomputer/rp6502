/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The .rp6502 loader against staged memory: the testbench plays the
 * platform bridge — it fills the staging window with a ROM image, posts
 * the slot length, and answers the machine's staging reads, the way the
 * APF data slot will. The firmware parses the records, refuses to touch
 * $FF00-$FFF9, lands the vectors in the register cells, and releases the
 * 6502 into the staged program. The variants drive the platform's real
 * ports: the slot_set sideband instead of the register poke, and a slow
 * staging answer holding stage_stall the way SDRAM will.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "utest.h"

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

static void rom_record(std::vector<uint8_t> &rom, uint16_t addr,
                       const uint8_t *data, size_t len)
{
    char line[64];
    snprintf(line, sizeof(line), "$%04X $%zX $%08X\n",
             addr, len, crc32_buf(data, len));
    rom.insert(rom.end(), line, line + strlen(line));
    rom.insert(rom.end(), data, data + len);
}

/* Build the two-record image every case stages. */
static std::vector<uint8_t> make_rom()
{
    /* The staged program: print "RP" under the $FFE0 ready bit, STP. */
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
    rom_record(rom, 0x0300, prog, sizeof(prog));
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));
    return rom;
}

/* Boot the staged image and demand the printed proof. slot_by_port
 * posts the length through the sideband; stall_cycles answers staging
 * reads like a memory that needs that many clocks per byte. */
static void run_staged(int *utest_result, bool slot_by_port,
                       int stall_cycles)
{
    std::vector<uint8_t> rom = make_rom();

    dut->rst_n = 0;
    for (int i = 0; i < 4; i++)
    {
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
    }
    dut->rst_n = 1;
    /* Reset clears the slot register; the bridge posts it afterward. */
    if (slot_by_port)
    {
        dut->slot_len = (uint32_t)rom.size();
        dut->slot_set = 1;
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
        dut->slot_set = 0;
    }
    else
        dut->rootp->rp6502__DOT__rv__DOT__mmio_slot_len =
            (uint32_t)rom.size();

    std::string rv_out, cpu_out;
    bool stopped = false;
    int stalled = 0;
    for (int i = 0; i < 64000000; i++)
    {
        uint32_t a = dut->rp6502_stage_addr;
        if (stall_cycles && dut->rp6502_stage_pend)
        {
            /* The byte stands only when the stall drops, like a
             * controller finishing its read. */
            if (stalled < stall_cycles)
            {
                dut->stage_stall = 1;
                stalled++;
            }
            else
            {
                dut->stage_stall = 0;
                dut->stage_rdata = a < rom.size() ? rom[a] : 0;
            }
        }
        else
        {
            dut->stage_stall = 0;
            dut->stage_rdata = a < rom.size() ? rom[a] : 0;
            stalled = 0;
        }
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
        if (!dut->rp6502_stage_pend)
            stalled = 0;
        if (dut->rp6502_rv_tx_valid)
            rv_out.push_back((char)dut->rp6502_rv_tx_data);
        if (dut->rp6502_tx_valid)
            cpu_out.push_back((char)dut->rp6502_tx_data);
        stopped = dut->rootp->rp6502__DOT__cpu__DOT__stop_flag != 0;
        if (dut->rp6502_rv_halted && stopped)
            break;
    }

    if (getenv("BOOT_DEBUG"))
        fprintf(stderr, "cpu_out=[%s]\nrv_out=[%s]\nhalted=%d stopped=%d\n",
                cpu_out.c_str(), rv_out.c_str(),
                (int)dut->rp6502_rv_halted, (int)stopped);
    ASSERT_TRUE(dut->rp6502_rv_halted);
    ASSERT_EQ(dut->rp6502_rv_exit_code, 0u);
    ASSERT_TRUE(stopped);
    ASSERT_STREQ(cpu_out.c_str(), "RP");
    ASSERT_TRUE(strstr(rv_out.c_str(), "rom: staged") != NULL);
}

UTEST(rom, staged_rom_boots)
{
    ASSERT_TRUE(load_firmware(SW_BIN));
    run_staged(utest_result, false, 0);
}

UTEST(rom, slot_posted_by_port)
{
    ASSERT_TRUE(load_firmware(SW_BIN));
    run_staged(utest_result, true, 0);
}

UTEST(rom, staging_stalls_like_sdram)
{
    ASSERT_TRUE(load_firmware(SW_BIN));
    run_staged(utest_result, true, 12);
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
