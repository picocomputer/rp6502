/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef _TESTS_FPGA_TB_STAGE_H_
#define _TESTS_FPGA_TB_STAGE_H_

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

/* stage_map_gate.py checks these against the firmware's mmio.h and the
 * Pocket core's data.json. */
#define TB_STAGE_ROM_BASE 0x00000000u
/* The soft CPU's memory. Its ROM block is first, where the host loads
 * the firmware, and its RAM block follows. */
#define TB_STAGE_MEM_BASE 0x03E00000u
#define TB_STAGE_MEM_SIZE 0x00080000u
#define TB_STAGE_MEM_ROM_SIZE 0x00040000u
#define TB_STAGE_MEM_RAM_SIZE 0x00010000u
#define TB_STAGE_WIN_BASE 0x03E80000u
#define TB_STAGE_WIN_SIZE 0x00008000u

static const std::vector<uint8_t> &tb_stage_load(const char *path,
                                                 std::vector<uint8_t> &v)
{
    if (!v.empty())
        return v;
    FILE *f = fopen(path, "rb");
    if (!f)
        return v;
    uint8_t buf[4096];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        v.insert(v.end(), buf, buf + n);
    fclose(f);
    return v;
}

/* Puts bytes straight into an SDRAM model's memory, which holds two bytes
 * a word with the even byte low. Streaming them over the bridge costs
 * about ten clk_74a cycles a byte. */
template <typename Mem>
static void tb_stage_poke(Mem &mem, uint32_t base,
                          const std::vector<uint8_t> &v)
{
    for (size_t i = 0; i < v.size(); i += 2)
    {
        uint8_t hi = i + 1 < v.size() ? v[i + 1] : 0;
        mem[(base + i) >> 1] = (uint16_t)(v[i] | (hi << 8));
    }
}

/* The firmware image, or nothing when the file is missing or is larger
 * than the ROM block. */
static std::vector<uint8_t> tb_stage_firmware(const char *path)
{
    std::vector<uint8_t> v;
    tb_stage_load(path, v);
    if (v.size() > TB_STAGE_MEM_ROM_SIZE)
        v.clear();
    return v;
}

/* Loads the firmware where the host would, without the bridge. */
template <typename Mem>
static bool tb_stage_poke_firmware(Mem &mem, const char *path)
{
    std::vector<uint8_t> v = tb_stage_firmware(path);
    tb_stage_poke(mem, TB_STAGE_MEM_BASE, v);
    return !v.empty();
}

static std::map<uint32_t, uint8_t> &tb_stage_store()
{
    static std::map<uint32_t, uint8_t> m;
    return m;
}

static void tb_stage_write(uint32_t addr, uint8_t b)
{
    tb_stage_store()[addr] = b;
}

static void tb_stage_clear()
{
    tb_stage_store().clear();
}

static uint8_t tb_stage(const std::vector<uint8_t> &rom, uint32_t addr);

/* The machine takes the staging store a word at a time, the lowest
 * address in the low byte. */
static uint32_t tb_stage_word(const std::vector<uint8_t> &rom, uint32_t addr)
{
    addr &= ~3u;
    uint32_t w = 0;
    for (uint32_t i = 0; i < 4; i++)
        w |= (uint32_t)tb_stage(rom, addr + i) << (8 * i);
    return w;
}

/* A write of the machine's, the bytes strb names of the word at addr,
 * the lowest address in the low byte. */
static void tb_stage_write_word(uint32_t addr, uint32_t data, uint32_t strb)
{
    addr &= ~3u;
    for (uint32_t i = 0; i < 4; i++)
        if (strb & (1u << i))
            tb_stage_write(addr + i, (uint8_t)(data >> (8 * i)));
}

/* What a platform shows while no access is pending, which the machine
 * must not use. */
#define TB_STAGE_IDLE 0xDEADBEEFu

static uint8_t tb_stage(const std::vector<uint8_t> &rom, uint32_t addr)
{
    std::map<uint32_t, uint8_t> &m = tb_stage_store();
    std::map<uint32_t, uint8_t>::const_iterator it = m.find(addr);
    if (it != m.end())
        return it->second;
    if (addr >= TB_STAGE_ROM_BASE
        && addr - TB_STAGE_ROM_BASE < rom.size())
        return rom[addr - TB_STAGE_ROM_BASE];
    return 0;
}

/* Called before every clock. It takes the machine's write, if one is
 * pending, and shows the word a pending read wants. */
template <typename Dut>
static void tb_stage_tick(Dut *dut, const std::vector<uint8_t> &rom)
{
    uint32_t at = dut->wiring_stage_addr;
    if (dut->wiring_stage_pend && dut->wiring_stage_we)
        tb_stage_write_word(at, dut->wiring_stage_wdata,
                            dut->wiring_stage_wstrb);
    dut->stage_word = dut->wiring_stage_pend ? tb_stage_word(rom, at)
                                             : TB_STAGE_IDLE;
}

#endif /* _TESTS_FPGA_TB_STAGE_H_ */
