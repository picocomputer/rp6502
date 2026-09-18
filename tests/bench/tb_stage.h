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
#define TB_STAGE_WIN_BASE 0x03FA0000u
#define TB_STAGE_WIN_SIZE 0x00008000u
#define TB_STAGE_FONT_BASE 0x03FE0000u
#define TB_STAGE_FONT_SIZE 0x0000F000u
#define TB_STAGE_OEMCP_BASE 0x03FEF000u
#define TB_STAGE_OEMCP_SIZE 0x00002000u
/* The Get File scratch window at 0x03FF1000, which is not a data slot,
 * lies between the code pages and this base. */
#define TB_STAGE_KBDLAY_BASE 0x03FF2000u
#define TB_STAGE_KBDLAY_SIZE 0x00004000u

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

static const std::vector<uint8_t> &tb_stage_fonts()
{
    static std::vector<uint8_t> v;
    return tb_stage_load(FONTS_BIN, v);
}

static const std::vector<uint8_t> &tb_stage_oemcp()
{
    static std::vector<uint8_t> v;
    return tb_stage_load(OEMCP_BIN, v);
}

static const std::vector<uint8_t> &tb_stage_kbdlay()
{
    static std::vector<uint8_t> v;
    return tb_stage_load(KBDLAY_BIN, v);
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

static uint8_t tb_stage(const std::vector<uint8_t> &rom, uint32_t addr)
{
    std::map<uint32_t, uint8_t> &m = tb_stage_store();
    std::map<uint32_t, uint8_t>::const_iterator it = m.find(addr);
    if (it != m.end())
        return it->second;
    if (addr >= TB_STAGE_FONT_BASE
        && addr < TB_STAGE_FONT_BASE + TB_STAGE_FONT_SIZE)
    {
        const std::vector<uint8_t> &fonts = tb_stage_fonts();
        uint32_t i = addr - TB_STAGE_FONT_BASE;
        return i < fonts.size() ? fonts[i] : 0;
    }
    if (addr >= TB_STAGE_OEMCP_BASE
        && addr < TB_STAGE_OEMCP_BASE + TB_STAGE_OEMCP_SIZE)
    {
        const std::vector<uint8_t> &t = tb_stage_oemcp();
        uint32_t i = addr - TB_STAGE_OEMCP_BASE;
        return i < t.size() ? t[i] : 0;
    }
    if (addr >= TB_STAGE_KBDLAY_BASE
        && addr < TB_STAGE_KBDLAY_BASE + TB_STAGE_KBDLAY_SIZE)
    {
        const std::vector<uint8_t> &t = tb_stage_kbdlay();
        uint32_t i = addr - TB_STAGE_KBDLAY_BASE;
        return i < t.size() ? t[i] : 0;
    }
    if (addr >= TB_STAGE_ROM_BASE
        && addr - TB_STAGE_ROM_BASE < rom.size())
        return rom[addr - TB_STAGE_ROM_BASE];
    return 0;
}

#endif /* _TESTS_FPGA_TB_STAGE_H_ */
