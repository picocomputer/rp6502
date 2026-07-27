/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The M2 exit test: one .rp6502 image, two machines. The RTL machine runs
 * it from the staged window under the cross-compiled firmware; emu_core
 * runs it as the reference oracle. The program exercises the syscall ABI —
 * UART bytes, an xstack argument into a real std op, the AX return, the
 * ENOSYS errno path — and both consoles must carry identical bytes.
 */

#include "Vrp6502.h"
#include "Vrp6502___024root.h"

#include "oracle.h"
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

UTEST(compare, syscall_rom_matches_oracle)
{
    std::vector<uint8_t> rom;
    const char magic[] = "#!RP6502\n";
    rom.insert(rom.end(), magic, magic + strlen(magic));
    rom_record(rom, 0x0300, prog, sizeof(prog));
    rom_record(rom, 0xFFFC, vectors, sizeof(vectors));

    /* The oracle side: the same image from a file, console tapped. */
    const char *path = "test_compare.rp6502";
    FILE *f = fopen(path, "wb");
    ASSERT_TRUE(f != NULL);
    fwrite(rom.data(), 1, rom.size(), f);
    fclose(f);
    oracle_init();
    oracle_tap_start();
    ASSERT_TRUE(oracle_restart(path));
    oracle_run_frames(30);
    size_t tap_len;
    const char *tap = oracle_tap_data(&tap_len);
    std::string oracle_out(tap, tap_len);

    /* The RTL side: the same image from the staged window. */
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

    std::string rv_out;
    bool stopped = false;
    for (int i = 0; i < 800000; i++)
    {
        uint32_t a = dut->rp6502_stage_addr;
        dut->stage_rdata = a < rom.size() ? rom[a] : 0;
        dut->clk_sys = 1;
        dut->eval();
        dut->clk_sys = 0;
        dut->eval();
        if (dut->rp6502_rv_tx_valid)
            rv_out.push_back((char)dut->rp6502_rv_tx_data);
        stopped = dut->rootp->rp6502__DOT__cpu__DOT__stop_flag != 0;
        if (dut->rp6502_rv_halted && stopped)
            break;
    }
    ASSERT_TRUE(dut->rp6502_rv_halted);
    ASSERT_TRUE(stopped);

    /* The machine's stream begins after the boot narration; the oracle's
     * may begin with its startup banner. The program bytes must match
     * exactly: same UART bytes, same std write, same AX, same errno. */
    const char *marker = "boot: running\n";
    size_t at = rv_out.find(marker);
    ASSERT_TRUE(at != std::string::npos);
    std::string rtl_stream = rv_out.substr(at + strlen(marker));
    ASSERT_TRUE(rtl_stream.size() >= 7);
    ASSERT_TRUE(oracle_out.size() >= rtl_stream.size());
    ASSERT_STREQ(oracle_out.substr(oracle_out.size() - rtl_stream.size()).c_str(),
                 rtl_stream.c_str());
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