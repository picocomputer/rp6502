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
/* The soft CPU's RAM, where the host loads the firmware. */
#define TB_STAGE_RAM_BASE 0x03EC0000u
#define TB_STAGE_RAM_SIZE 0x00040000u
#define TB_STAGE_WIN_BASE 0x03FA0000u
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
 * than the soft CPU's RAM. */
static std::vector<uint8_t> tb_stage_firmware(const char *path)
{
    std::vector<uint8_t> v;
    tb_stage_load(path, v);
    if (v.size() > TB_STAGE_RAM_SIZE)
        v.clear();
    return v;
}

/* Loads the firmware where the host would, without the bridge. */
template <typename Mem>
static bool tb_stage_poke_firmware(Mem &mem, const char *path)
{
    std::vector<uint8_t> v = tb_stage_firmware(path);
    tb_stage_poke(mem, TB_STAGE_RAM_BASE, v);
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

/* The machine takes the staging store a halfword at a time and picks the
 * byte itself. */
static uint16_t tb_stage_half(const std::vector<uint8_t> &rom, uint32_t addr)
{
    addr &= ~1u;
    return (uint16_t)(tb_stage(rom, addr) | (tb_stage(rom, addr + 1) << 8));
}

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

#endif /* _TESTS_FPGA_TB_STAGE_H_ */
