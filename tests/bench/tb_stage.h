/*
 * Copyright (c) 2026 Rumbledethumps
 *
 * SPDX-License-Identifier: BSD-3-Clause
 *
 * The staging store, as the platform presents it. The picked image is a
 * whole copy the host pushes to TB_STAGE_ROM_BASE; the fonts, the code
 * page tables and the keyboard layouts are whole files the host places
 * once and the machine reads at random; a descriptor's bytes arrive
 * through that slot's own 32 KB window, written by the host when it
 * performs a read.
 *
 * The assets are the core's own files, not fixtures, so they load once
 * and every test shares them.
 */

#ifndef _TESTS_FPGA_TB_STAGE_H_
#define _TESTS_FPGA_TB_STAGE_H_

#include <cstdint>
#include <cstdio>
#include <map>
#include <vector>

/* All of these must agree with the firmware's mmio.h and with data.json,
 * whose list order, slot ids, and addresses climb the SDRAM together —
 * the ROM first at the store's base, because it is the primary slot. */
#define TB_STAGE_ROM_BASE 0x00000000u
#define TB_STAGE_WIN_BASE 0x03FA0000u
#define TB_STAGE_WIN_SIZE 0x00008000u
#define TB_STAGE_FONT_BASE 0x03FE0000u
#define TB_STAGE_FONT_SIZE 0x0000F000u
#define TB_STAGE_OEMCP_BASE 0x03FEF000u
#define TB_STAGE_OEMCP_SIZE 0x00002000u
/* Above Get File's scratch page at 0x03FF1000, which is not a slot. */
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

/* What the host has written into the windows. Sparse, because a test
 * touches a few kilobytes of a 288 KB span. */
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

/* The rom argument is the image as the host's boot push left it, at
 * TB_STAGE_ROM_BASE. The store's writes shadow it, the way a reload or
 * an exec pull overwrites the image on hardware. */
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
